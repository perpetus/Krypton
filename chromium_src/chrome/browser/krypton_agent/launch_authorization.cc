#include "chrome/browser/krypton_agent/launch_authorization.h"

#include <windows.h>

#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <utility>

#include "base/base_paths.h"
#include "base/base_paths_win.h"
#include "base/check.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_handle.h"
#include "base/win/scoped_localalloc.h"
#include "base/win/security_descriptor.h"
#include "base/win/win_util.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "crypto/hash.h"
#include "sandbox/win/src/app_container_base.h"

namespace krypton_agent {
namespace {

constexpr DWORD kResponseTimeoutMs = 10000;
constexpr char kAcknowledgment = 'K';

bool LogSandboxFailure(std::string_view operation, std::string_view reason) {
  LOG(WARNING) << "Krypton Agent " << operation << " failed [" << reason << "]";
  return false;
}

base::Lock& SandboxDirectoryLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::wstring ContainerName(std::string_view profile,
                           std::string_view instance) {
  // The Windows profile name is limited to 64 characters.
  return base::UTF8ToWide(
      base::StrCat({"Krypton.Agent.", profile, ".", instance.substr(0, 17)}));
}

bool IsLocalExecutable(const base::FilePath& path) {
  const auto& name = path.value();
  if (name.size() < 4 || !base::IsAsciiAlpha(name[0]) || name[1] != L':' ||
      name[2] != L'\\' ||
      GetDriveTypeW(name.substr(0, 3).c_str()) != DRIVE_FIXED ||
      !base::FilePath::CompareEqualIgnoreCase(path.Extension(), L".exe")) {
    return false;
  }
  constexpr std::array<std::wstring_view, 10> shells{
      L"cmd.exe",   L"powershell.exe", L"pwsh.exe",    L"bash.exe",
      L"sh.exe",    L"wsl.exe",        L"cscript.exe", L"wscript.exe",
      L"mshta.exe", L"rundll32.exe"};
  const auto basename = path.BaseName();
  if (std::ranges::any_of(shells, [&basename](auto shell) {
        return base::FilePath::CompareEqualIgnoreCase(basename.value(), shell);
      })) {
    return false;
  }
  DWORD type = 0;
  return GetBinaryTypeW(name.c_str(), &type) &&
         (type == SCS_32BIT_BINARY || type == SCS_64BIT_BINARY);
}

bool IsFixedPath(const base::FilePath& path) {
  const auto& name = path.value();
  return name.size() >= 4 && base::IsAsciiAlpha(name[0]) && name[1] == L':' &&
         name[2] == L'\\' && name.find(L':', 2) == std::wstring::npos &&
         !path.ReferencesParent() &&
         GetDriveTypeW(name.substr(0, 3).c_str()) == DRIVE_FIXED;
}

bool PinLocalPath(base::FilePath& path, bool directory) {
  base::FilePath resolved;
  if (!IsFixedPath(path) || !base::NormalizeFilePath(path, &resolved) ||
      !base::FilePath::CompareEqualIgnoreCase(path.value(), resolved.value())) {
    return false;
  }
  const auto attributes = GetFileAttributesW(resolved.value().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      (bool(attributes & FILE_ATTRIBUTE_DIRECTORY) != directory)) {
    return false;
  }
  path = std::move(resolved);
  return true;
}

bool IsHexName(std::string_view value, size_t length) {
  return value.size() == length && std::ranges::all_of(value, [](char ch) {
           return base::IsHexDigit(ch);
         });
}

std::map<std::wstring, size_t>& ActiveSandboxes() {
  static base::NoDestructor<std::map<std::wstring, size_t>> active;
  return *active;
}

void ReleaseSandbox(std::wstring key, base::FilePath policy) {
  base::AutoLock lock(SandboxDirectoryLock());
  auto& active = ActiveSandboxes();
  auto found = active.find(key);
  CHECK(found != active.end());
  if (--found->second == 0) {
    active.erase(found);
  }
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
      base::BindOnce(base::IgnoreResult(&base::DeletePathRecursively),
                     std::move(policy)));
}

std::string InstanceHash(std::string_view instance) {
  return base::HexEncode(crypto::hash::Sha256(instance));
}

bool PinCliHome(base::FilePath& path) {
  base::FilePath home;
  return base::PathService::Get(base::DIR_HOME, &home) &&
         PinLocalPath(home, /*directory=*/true) &&
         PinLocalPath(path, /*directory=*/true) && home.IsParent(path);
}

bool SetCliAccess(base::FilePath path,
                  const base::win::Sid& sid,
                  bool directory,
                  DWORD allow,
                  DWORD deny,
                  bool remove) {
  if (!PinLocalPath(path, directory)) {
    return false;
  }
  constexpr SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION;
  auto descriptor = base::win::SecurityDescriptor::FromFile(path, info);
  if (!descriptor || !descriptor->dacl() || descriptor->dacl()->is_null()) {
    return false;
  }
  using Mode = base::win::SecurityAccessMode;
  const DWORD inherit =
      directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0;
  if (remove) {
    if (!descriptor->SetDaclEntry(sid, Mode::kRevoke, 0, 0)) {
      return false;
    }
  } else if (!descriptor->SetDaclEntry(sid, Mode::kSet, allow, inherit) ||
             (deny &&
              !descriptor->SetDaclEntry(sid, Mode::kDeny, deny, inherit))) {
    return false;
  }
  auto relative = descriptor->ToSelfRelative();
  return relative &&
         SetFileSecurityW(path.value().c_str(), info, relative->get());
}

bool ApplyCliGrants(base::FilePath home,
                    std::string_view driver,
                    const base::win::Sid& sid,
                    bool remove) {
  const bool claude = driver == "claude";
  if (!claude && driver != "codex") {
    return LogSandboxFailure("cli_acl", "unknown_driver");
  }
  if (!PinCliHome(home)) {
    return (remove && !base::PathExists(home)) ||
           LogSandboxFailure("cli_acl", "invalid_home");
  }
  constexpr DWORD kRead = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
  constexpr DWORD kWrite = FILE_GENERIC_WRITE;
  constexpr DWORD kDenyWrite = FILE_WRITE_DATA | FILE_APPEND_DATA |
                               FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE;
  const auto configure = [&](const wchar_t* name, bool directory, DWORD allow,
                             DWORD deny = 0) {
    const auto path = home.Append(name);
    return !base::PathExists(path) ||
           SetCliAccess(path, sid, directory, allow, deny, remove) ||
           LogSandboxFailure("cli_acl", "entry_acl");
  };
  bool ok = true;
  if (claude) {
    for (const auto* name :
         {L"settings.json", L"settings.local.json", L"CLAUDE.md"}) {
      ok &= configure(name, false, kRead, kDenyWrite);
    }
    for (const auto* name : {L"plugins", L"skills", L"agents", L"commands"}) {
      ok &= configure(name, true, kRead, kDenyWrite | FILE_DELETE_CHILD);
    }
    for (const auto* name : {L".credentials.json", L"history.jsonl"}) {
      ok &= configure(name, false, kRead | kWrite);
    }
  } else {
    for (const auto* name : {L"config.toml", L"AGENTS.md"}) {
      ok &= configure(name, false, kRead, kDenyWrite);
    }
    for (const auto* name : {L"skills", L"rules"}) {
      ok &= configure(name, true, kRead, kDenyWrite | FILE_DELETE_CHILD);
    }
    for (const auto* name : {L"auth.json", L"history.jsonl", L"state_5.sqlite",
                             L"state_5.sqlite-shm", L"state_5.sqlite-wal"}) {
      ok &= configure(name, false, kRead | kWrite);
    }
  }
  if (ok || remove) {
    ok &= SetCliAccess(home, sid, /*directory=*/true, kRead | kWrite, 0,
                       remove) ||
          LogSandboxFailure("cli_acl", "home_acl");
  }
  return ok || LogSandboxFailure("cli_acl", "grant_incomplete");
}

bool SameAcl(ACL* actual, ACL* expected) {
  if (!actual || !expected || actual->AceCount != expected->AceCount) {
    return false;
  }
  for (DWORD index = 0; index < actual->AceCount; ++index) {
    void* actual_entry = nullptr;
    void* expected_entry = nullptr;
    if (!GetAce(actual, index, &actual_entry) ||
        !GetAce(expected, index, &expected_entry)) {
      return false;
    }
    const auto size = static_cast<ACE_HEADER*>(actual_entry)->AceSize;
    if (size != static_cast<ACE_HEADER*>(expected_entry)->AceSize ||
        std::memcmp(actual_entry, expected_entry, size) != 0) {
      return false;
    }
  }
  return true;
}

bool EnsureDirectory(const base::FilePath& path,
                     base::win::SecurityDescriptor& expected,
                     bool migrate = false) {
  auto descriptor = expected.ToSelfRelative();
  if (!descriptor) {
    return LogSandboxFailure("directory_acl", "security_descriptor");
  }
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor->get(),
                                 FALSE};
  if (!CreateDirectoryW(path.value().c_str(), &attributes) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    PLOG(WARNING) << "Krypton Agent directory_acl failed [create_directory]";
    return false;
  }
  base::win::ScopedHandle directory(CreateFileW(
      path.value().c_str(),
      FILE_READ_ATTRIBUTES | READ_CONTROL |
          (migrate ? WRITE_DAC | (expected.sacl() ? WRITE_OWNER : 0) : 0),
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr));
  FILE_ATTRIBUTE_TAG_INFO info{};
  if (!directory.is_valid() ||
      !GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                    &info, sizeof(info))) {
    PLOG(WARNING) << "Krypton Agent directory_acl failed [directory_handle]";
    return false;
  }
  if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return LogSandboxFailure("directory_acl", "directory_attributes");
  }
  const SECURITY_INFORMATION security =
      DACL_SECURITY_INFORMATION |
      (expected.sacl() ? LABEL_SECURITY_INFORMATION : 0);
  auto actual = base::win::SecurityDescriptor::FromHandle(
      directory.get(), base::win::SecurityObjectType::kFile,
      security | (migrate ? OWNER_SECURITY_INFORMATION : 0));
  std::wstring user;
  if (migrate &&
      (!actual || !actual->owner() || !base::win::GetUserSidString(&user) ||
       actual->owner()->ToSddlString() != user)) {
    return LogSandboxFailure("directory_acl", "owner_check");
  }
  if (actual && actual->dacl_protected() && actual->dacl() && expected.dacl() &&
      SameAcl(actual->dacl()->get(), expected.dacl()->get()) &&
      (!expected.sacl() ||
       (actual->sacl() &&
        SameAcl(actual->sacl()->get(), expected.sacl()->get())))) {
    return true;
  }
  if (!migrate) {
    return LogSandboxFailure("directory_acl", "descriptor_mismatch");
  }
  if (!expected.WriteToHandle(directory.get(),
                              base::win::SecurityObjectType::kFile, security)) {
    PLOG(WARNING) << "Krypton Agent directory_acl failed [migration]";
    return false;
  }
  return true;
}

// Grant only the selected executable. Keep its handle so the grant cannot
// follow a replacement, and drop the grant after the supervisor job stops.
bool SetProgramAccess(HANDLE handle,
                      const base::win::Sid& sid,
                      bool directory,
                      base::win::SecurityAccessMode mode) {
  constexpr auto type = base::win::SecurityObjectType::kFile;
  constexpr SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION;
  constexpr DWORD kProgramRights = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
  auto descriptor =
      base::win::SecurityDescriptor::FromHandle(handle, type, info);
  return descriptor && descriptor->dacl() && !descriptor->dacl()->is_null() &&
         descriptor->SetDaclEntry(
             sid, mode, kProgramRights,
             directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0) &&
         descriptor->WriteToHandle(handle, type, info);
}

base::win::ScopedHandle OpenProgramForGrant(const base::FilePath& path,
                                            bool directory) {
  base::win::ScopedHandle file(
      CreateFileW(path.value().c_str(), READ_CONTROL | WRITE_DAC,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT |
                      (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0),
                  nullptr));
  FILE_ATTRIBUTE_TAG_INFO info{};
  if (!file.is_valid() ||
      !GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &info,
                                    sizeof(info)) ||
      (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      (bool(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)) {
    return {};
  }
  return file;
}

class ProgramAccess {
 public:
  ProgramAccess(const ProviderSandbox& sandbox,
                base::FilePath path,
                bool directory = false)
      : sandbox_(sandbox), path_(std::move(path)), directory_(directory) {}

  ~ProgramAccess() {
    if (key_.empty()) {
      return;
    }
    base::AutoLock lock(SandboxDirectoryLock());
    auto& grants = Grants();
    auto grant = grants.find(key_);
    if (grant == grants.end() || --grant->second.references != 0) {
      return;
    }
    if (SetProgramAccess(grant->second.file.get(), *sandbox_.sid, directory_,
                         base::win::SecurityAccessMode::kRevoke)) {
      base::DeleteFile(grant->second.record);
    } else {
      LOG(ERROR) << "Krypton Agent could not revoke provider executable access";
    }
    grants.erase(grant);
  }

  bool Acquire() {
    if (!sandbox_.sid || path_.empty()) {
      return true;
    }
    base::AutoLock lock(SandboxDirectoryLock());
    auto key = base::StrCat({sandbox_.name, L"|", path_.value()});
    for (auto& ch : key) {
      ch = base::ToLowerASCII(ch);
    }
    auto& grants = Grants();
    if (auto grant = grants.find(key); grant != grants.end()) {
      ++grant->second.references;
      key_ = std::move(key);
      return true;
    }
    auto container = sandbox::AppContainerBase::Open(sandbox_.name);
    if (!container) {
      return false;
    }
    container->SetEnableLowPrivilegeAppContainer(sandbox_.isolation ==
                                                 ProviderIsolation::kLpac);
    DWORD granted = 0;
    BOOL allowed = FALSE;
    if (container->AccessCheck(path_.value(),
                               base::win::SecurityObjectType::kFile,
                               kProgramRights, &granted, &allowed) &&
        allowed) {
      return true;
    }
    auto path = path_;
    if (!PinLocalPath(path, directory_)) {
      return false;
    }
    auto file = OpenProgramForGrant(path, directory_);
    auto record = sandbox_.grant_directory.AppendASCII(
        InstanceHash(base::WideToUTF8(key)));
    const auto payload = base::WriteJson(base::DictValue()
                                             .Set("path", path.AsUTF8Unsafe())
                                             .Set("directory", directory_));
    if (!file.is_valid() || !payload ||
        (base::PathExists(record) && !PinLocalPath(record, false)) ||
        !base::ImportantFileWriter::WriteFileAtomically(record, *payload) ||
        !SetProgramAccess(file.get(), *sandbox_.sid, directory_,
                          base::win::SecurityAccessMode::kGrant)) {
      return false;
    }
    key_ = std::move(key);
    grants.emplace(key_, Grant{std::move(file), std::move(record), 1});
    return true;
  }

 private:
  static constexpr DWORD kProgramRights =
      FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;

  struct Grant {
    base::win::ScopedHandle file;
    base::FilePath record;
    size_t references;
  };

  static std::map<std::wstring, Grant>& Grants() {
    static base::NoDestructor<std::map<std::wstring, Grant>> grants;
    return *grants;
  }

  const ProviderSandbox& sandbox_;
  const base::FilePath path_;
  const bool directory_;
  std::wstring key_;
};

bool PrepareDirectories(const base::FilePath& local,
                        std::string_view profile,
                        std::string_view instance,
                        const base::win::Sid* package,
                        base::FilePath& work,
                        base::FilePath& config) {
  std::wstring user;
  const auto sid = package ? package->ToSddlString() : std::nullopt;
  if ((package && !sid) || !base::win::GetUserSidString(&user)) {
    return LogSandboxFailure("provider_directories", "identity");
  }

  // Writable roots grant delete only to descendants. The skills directory
  // has a separate protected DACL so agents cannot move, replace, or edit it.
  const auto user_acl = base::StrCat({L"D:P(A;OICI;0x1f01ff;;;", user, L")"});
  auto root_acl = base::win::SecurityDescriptor::FromSddl(user_acl);
  auto per_instance_acl = base::win::SecurityDescriptor::FromSddl(
      sid ? base::StrCat({user_acl, L"(A;;0x1201bf;;;", *sid,
                          L")(A;OICIIO;0x1301bf;;;", *sid,
                          L")S:(ML;OICI;NW;;;LW)"})
          : user_acl);
  auto skills_acl = base::win::SecurityDescriptor::FromSddl(
      sid ? base::StrCat({user_acl, L"(A;OICI;0x1200a9;;;", *sid, L")"})
          : user_acl);
  if (!root_acl || !per_instance_acl || !skills_acl) {
    return LogSandboxFailure("provider_directories", "sddl");
  }

  // The browser may have created these two parents for the opt-in component.
  const auto root = local.Append(L"Krypton").Append(L"Agent");
  base::FilePath final_root;
  if (!base::NormalizeFilePath(root, &final_root) ||
      !base::FilePath::CompareEqualIgnoreCase(root.value(),
                                              final_root.value())) {
    return LogSandboxFailure("provider_directories", "agent_root");
  }
  for (const auto* name : {L"work", L"config"}) {
    const auto type_root = root.Append(name);
    const auto profile_root = type_root.AppendASCII(profile);
    const auto provider_root = profile_root.AppendASCII(instance);
    if (!EnsureDirectory(type_root, *root_acl, /*migrate=*/true) ||
        !EnsureDirectory(profile_root, *root_acl, /*migrate=*/true)) {
      return LogSandboxFailure("provider_directories", "parent_acl");
    }
    if (!EnsureDirectory(provider_root, *per_instance_acl, /*migrate=*/true)) {
      return LogSandboxFailure(
          "provider_directories",
          name == std::wstring_view(L"work") ? "work_acl" : "config_acl");
    }
    if (name == std::wstring_view(L"work")) {
      work = provider_root;
    } else {
      config = provider_root;
    }
  }
  if (!EnsureDirectory(config.Append(L"skills"), *skills_acl,
                       /*migrate=*/true)) {
    return LogSandboxFailure("provider_directories", "skills_acl");
  }
  if (!EnsureDirectory(config.Append(L"AppData"), *per_instance_acl,
                       /*migrate=*/true) ||
      !EnsureDirectory(config.Append(L"AppData").Append(L"Roaming"),
                       *per_instance_acl, /*migrate=*/true)) {
    return LogSandboxFailure("provider_directories", "app_data_acl");
  }
  return true;
}

}  // namespace

bool IsLocalProviderUsageDatabase(std::string_view path) {
  if (path.empty() || path.size() > 32768 ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }
  auto file_path = base::FilePath::FromUTF8Unsafe(path);
  if (!PinLocalPath(file_path, /*directory=*/false)) {
    return false;
  }
  const base::win::ScopedHandle file(CreateFileW(
      file_path.value().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  FILE_ATTRIBUTE_TAG_INFO info{};
  return file.is_valid() && GetFileType(file.get()) == FILE_TYPE_DISK &&
         GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &info,
                                      sizeof(info)) &&
         !(info.FileAttributes &
           (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY |
            FILE_ATTRIBUTE_DEVICE));
}

bool PrepareAgentPrivateDirectories(const base::FilePath& local_app_data) {
  std::wstring user;
  if (!base::win::GetUserSidString(&user)) {
    return LogSandboxFailure("private_directories", "user_sid");
  }
  auto acl = base::win::SecurityDescriptor::FromSddl(
      base::StrCat({L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;", user, L")"}));
  const auto krypton = local_app_data.Append(L"Krypton");
  const auto root = krypton.Append(L"Agent");
  if (!base::CreateDirectory(krypton)) {
    return LogSandboxFailure("private_directories", "krypton_root");
  }
  auto fixed_krypton = krypton;
  if (!acl || !PinLocalPath(fixed_krypton, /*directory=*/true) ||
      !EnsureDirectory(root, *acl, /*migrate=*/true)) {
    return LogSandboxFailure("private_directories", "agent_root_acl");
  }
  for (const auto* name : {L"secrets", L"history", L"logs", L"t3", L"dev",
                           L"tokens", L"policy", L"grants", L"containers"}) {
    if (!EnsureDirectory(root.Append(name), *acl, /*migrate=*/true)) {
      LOG(WARNING) << "Krypton Agent private_directories failed [child_acl] "
                   << base::WideToASCII(name);
      return false;
    }
  }
  return true;
}

std::string_view IsolationName(ProviderIsolation isolation) {
  switch (isolation) {
    case ProviderIsolation::kAppContainer:
      return "app_container";
    case ProviderIsolation::kLpac:
      return "lpac";
    case ProviderIsolation::kJob:
      return "job";
  }
}

std::optional<ProviderSandbox> PrepareProviderSandbox(
    ProviderProgram program,
    std::string_view profile_token,
    std::string_view provider_instance,
    ProviderIsolation isolation,
    std::string_view* failure) {
  const auto reject =
      [failure](std::string_view reason) -> std::optional<ProviderSandbox> {
    if (failure) {
      *failure = reason;
    }
    LogSandboxFailure("prepare_sandbox", reason);
    return std::nullopt;
  };
  if (!IsHexName(profile_token, 32) || provider_instance.empty() ||
      provider_instance.size() > 256) {
    return reject("provider_identity");
  }
  base::FilePath local;
  if (!PinLocalPath(program.executable, false)) {
    return reject("program_path");
  }
  if (!IsLocalExecutable(program.executable)) {
    return reject("program_binary");
  }
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
    return reject("local_app_data");
  }
  if (!PrepareAgentPrivateDirectories(local)) {
    return reject("private_directories");
  }
  if (!program.script.empty() || !program.package_root.empty()) {
    if (!base::FilePath::CompareEqualIgnoreCase(
            program.executable.BaseName().value(), L"node.exe") ||
        !PinLocalPath(program.script, false) ||
        !PinLocalPath(program.package_root, true) ||
        !program.package_root.IsParent(program.script) ||
        !base::FilePath::CompareEqualIgnoreCase(
            program.package_root.BaseName().value(), L"gemini-cli") ||
        !base::FilePath::CompareEqualIgnoreCase(
            program.package_root.DirName().BaseName().value(), L"@google")) {
      return reject("interpreter_package");
    }
  }

  const std::string profile = base::ToLowerASCII(profile_token);
  base::AutoLock lock(SandboxDirectoryLock());
  const std::string instance = InstanceHash(provider_instance);
  const auto records = local.Append(L"Krypton")
                           .Append(L"Agent")
                           .Append(L"containers")
                           .AppendASCII(profile);
  std::wstring owner;
  if (!base::win::GetUserSidString(&owner)) {
    return reject("record_owner");
  }
  auto record_acl = base::win::SecurityDescriptor::FromSddl(
      base::StrCat({L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;", owner, L")"}));
  if (!record_acl || !EnsureDirectory(records, *record_acl, /*migrate=*/true) ||
      !EnsureDirectory(records.AppendASCII(instance), *record_acl,
                       /*migrate=*/true)) {
    return reject("container_record_acl");
  }
  const auto key = ContainerName(profile, instance);
  std::wstring name;
  std::optional<base::win::Sid> sid;
  base::FilePath container_directory;
  if (isolation != ProviderIsolation::kJob) {
    name = key;
    auto container =
        sandbox::AppContainerBase::CreateProfile(name, L"Krypton Agent");
    if (!container) {
      return reject("app_container_profile");
    }
    sid = container->GetPackageSid().Clone();
    const auto sid_string = sid->ToSddlString();
    base::win::ScopedCoMem<wchar_t> folder;
    if (!sid_string) {
      return reject("app_container_sid");
    }
    const HRESULT result =
        GetAppContainerFolderPath(sid_string->c_str(), &folder);
    if (FAILED(result) || !folder.get()) {
      LOG(WARNING) << "Krypton Agent app_container_folder HRESULT " << result;
      return reject("app_container_folder");
    }
    container_directory = base::FilePath(folder.get());
    const auto temp = container_directory.Append(L"Temp");
    if (!PinLocalPath(container_directory, /*directory=*/true) ||
        !base::CreateDirectory(temp) || base::IsLink(temp)) {
      return reject("app_container_temp");
    }
  }
  base::FilePath work;
  base::FilePath config;
  if (!PrepareDirectories(local, profile, instance, sid ? &*sid : nullptr, work,
                          config)) {
    return reject("provider_directories");
  }
  if (!program.cli_directory.empty()) {
    if (!sid || !PinCliHome(program.cli_directory)) {
      return reject("cli_home");
    }
    const auto grants = local.Append(L"Krypton")
                            .Append(L"Agent")
                            .Append(L"grants")
                            .AppendASCII(profile);
    std::wstring user;
    if (!base::win::GetUserSidString(&user)) {
      return reject("cli_grant_owner");
    }
    auto grants_acl = base::win::SecurityDescriptor::FromSddl(
        base::StrCat({L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;", user, L")"}));
    auto record = grants.AppendASCII(instance);
    const auto payload =
        base::WriteJson(base::DictValue()
                            .Set("home", program.cli_directory.AsUTF8Unsafe())
                            .Set("driver", program.driver));
    std::string previous;
    if (!grants_acl ||
        !EnsureDirectory(grants, *grants_acl, /*migrate=*/true) || !payload ||
        (base::PathExists(record) &&
         (!PinLocalPath(record, /*directory=*/false) ||
          !base::ReadFileToStringWithMaxSize(record, &previous, 32 * 1024) ||
          previous != *payload)) ||
        (previous.empty() &&
         !base::ImportantFileWriter::WriteFileAtomically(record, *payload))) {
      return reject("cli_grant_record");
    }
    if (!ApplyCliGrants(program.cli_directory, program.driver, *sid,
                        /*remove=*/false)) {
      return reject("cli_grants");
    }
  }
  std::wstring user;
  const auto sid_string = sid ? sid->ToSddlString() : std::nullopt;
  if ((sid && !sid_string) || !base::win::GetUserSidString(&user)) {
    return reject("policy_identity");
  }
  auto policy_acl = base::win::SecurityDescriptor::FromSddl(base::StrCat(
      {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;", user, L")",
       sid_string ? base::StrCat({L"(A;OICI;0x1200a9;;;", *sid_string, L")"})
                  : std::wstring()}));
  const auto policy =
      local.Append(L"Krypton")
          .Append(L"Agent")
          .Append(L"policy")
          .AppendASCII(base::HexEncode(base::RandBytesAsVector(16)));
  if (!policy_acl || !EnsureDirectory(policy, *policy_acl)) {
    return reject("policy_directory_acl");
  }
  ++ActiveSandboxes()[key];
  return ProviderSandbox{.program = std::move(program.executable),
                         .working_directory = std::move(work),
                         .config_directory = std::move(config),
                         .container_directory = std::move(container_directory),
                         .policy_directory = policy,
                         .grant_directory = records.AppendASCII(instance),
                         .cli_directory = std::move(program.cli_directory),
                         .script = std::move(program.script),
                         .package_root = std::move(program.package_root),
                         .provider_id = std::string(provider_instance),
                         .name = std::move(name),
                         .sid = std::move(sid),
                         .isolation = isolation,
                         .lifetime = base::ScopedClosureRunner(
                             base::BindOnce(&ReleaseSandbox, key, policy))};
}

namespace {

bool RevokeProgramRecords(base::FilePath records, const base::win::Sid& sid) {
  if (!base::PathExists(records)) {
    return true;
  }
  if (!PinLocalPath(records, /*directory=*/true)) {
    return false;
  }
  base::FileEnumerator files(records, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  for (auto record = files.Next(); !record.empty(); record = files.Next()) {
    std::string json;
    if (!IsHexName(record.BaseName().MaybeAsASCII(), 64) ||
        !PinLocalPath(record, /*directory=*/false) ||
        !base::ReadFileToStringWithMaxSize(record, &json, 32 * 1024)) {
      return false;
    }
    const auto data = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    const auto* file_path = data ? data->FindString("path") : nullptr;
    const auto directory = data ? data->FindBool("directory") : std::nullopt;
    if (!file_path || !directory) {
      return false;
    }
    auto file = base::FilePath::FromUTF8Unsafe(*file_path);
    if (base::PathExists(file)) {
      if (!PinLocalPath(file, *directory)) {
        return false;
      }
      auto handle = OpenProgramForGrant(file, *directory);
      if (!handle.is_valid() ||
          !SetProgramAccess(handle.get(), sid, *directory,
                            base::win::SecurityAccessMode::kRevoke)) {
        return false;
      }
    }
    if (!base::DeleteFile(record)) {
      return false;
    }
  }
  return true;
}

void SweepProviderSandboxes(const base::FilePath& local_app_data,
                            std::string_view profile_token,
                            const std::vector<std::string>& instances,
                            bool explicit_removal,
                            base::ListValue* diagnostics) {
  if (!IsHexName(profile_token, 32) ||
      (!explicit_removal && instances.empty())) {
    return;
  }
  base::AutoLock lock(SandboxDirectoryLock());
  const auto root = local_app_data.Append(L"Krypton").Append(L"Agent");
  auto safe_root = root;
  if (!PinLocalPath(safe_root, /*directory=*/true)) {
    return;
  }
  const auto token = base::ToLowerASCII(profile_token);
  const auto config = root.Append(L"config").AppendASCII(token);
  const auto work = root.Append(L"work").AppendASCII(token);
  const auto grants = root.Append(L"grants").AppendASCII(token);
  const auto records = root.Append(L"containers").AppendASCII(token);
  std::vector<std::string> keep;
  base::ListValue inventory;
  for (const auto& instance : instances) {
    if (instance.empty() || instance.size() > 256) {
      return;
    }
    keep.push_back(base::ToUpperASCII(InstanceHash(instance)));
    inventory.Append(instance);
  }
  auto safe_records = records;
  if (!explicit_removal) {
    if (!PinLocalPath(safe_records, /*directory=*/true)) {
      return;
    }
    const auto file = safe_records.Append(L"inventory.json");
    auto validated_file = file;
    const auto json = base::WriteJson(inventory);
    if (!json ||
        (base::PathExists(file) &&
         !PinLocalPath(validated_file, /*directory=*/false)) ||
        !base::ImportantFileWriter::WriteFileAtomically(file, *json)) {
      LogSandboxFailure("sandbox_cleanup", "inventory_write");
      return;
    }
  }
  std::vector<std::string> stale;
  for (const auto& [folder, kind] :
       {std::pair{records, base::FileEnumerator::DIRECTORIES},
        std::pair{config, base::FileEnumerator::DIRECTORIES},
        std::pair{work, base::FileEnumerator::DIRECTORIES},
        std::pair{grants, base::FileEnumerator::FILES}}) {
    auto safe_folder = folder;
    if (base::PathExists(folder) &&
        !PinLocalPath(safe_folder, /*directory=*/true)) {
      return;
    }
    base::FileEnumerator paths(folder, /*recursive=*/false, kind);
    for (auto path = paths.Next(); !path.empty(); path = paths.Next()) {
      const auto hash = base::ToUpperASCII(path.BaseName().MaybeAsASCII());
      if (IsHexName(hash, 64) && !std::ranges::contains(keep, hash) &&
          !std::ranges::contains(stale, hash) && !base::IsLink(path)) {
        stale.push_back(hash);
      }
    }
  }
  for (const auto& hash : stale) {
    const auto name = ContainerName(token, hash);
    if (ActiveSandboxes().contains(name)) {
      continue;
    }
    auto config_dir = config.AppendASCII(hash);
    auto work_dir = work.AppendASCII(hash);
    if ((base::PathExists(config_dir) &&
         !PinLocalPath(config_dir, /*directory=*/true)) ||
        (base::PathExists(work_dir) &&
         !PinLocalPath(work_dir, /*directory=*/true))) {
      LogSandboxFailure("sandbox_cleanup", "provider_directory");
      continue;
    }
    const auto audit = [diagnostics, &hash](std::string_view item) {
      LOG(WARNING) << "Krypton Agent retired provider " << hash << " [" << item
                   << "]";
      if (diagnostics) {
        diagnostics->Append(base::DictValue()
                                .Set("event", "sandbox_removed")
                                .Set("instance_hash", hash)
                                .Set("item", item));
      }
    };
    auto container = sandbox::AppContainerBase::Open(name);
    if (!container || !RevokeProgramRecords(records.AppendASCII(hash),
                                            container->GetPackageSid())) {
      continue;
    }
    const auto record = grants.AppendASCII(hash);
    if (base::PathExists(record)) {
      std::string json;
      if (base::IsLink(record) ||
          !base::ReadFileToStringWithMaxSize(record, &json, 32 * 1024)) {
        continue;
      }
      const auto entry = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
      const auto* home = entry ? entry->FindString("home") : nullptr;
      const auto* driver = entry ? entry->FindString("driver") : nullptr;
      if (!home || !driver ||
          !ApplyCliGrants(base::FilePath::FromUTF8Unsafe(*home), *driver,
                          container->GetPackageSid(), /*remove=*/true) ||
          !base::DeleteFile(record)) {
        continue;
      }
      audit("cli_grants");
    }
    if (sandbox::AppContainerBase::ProfileExists(name)) {
      if (!sandbox::AppContainerBase::Delete(name)) {
        continue;
      }
      audit("app_container");
    }
    if (base::PathExists(config_dir) &&
        base::DeletePathRecursively(config_dir)) {
      audit("config");
    }
    if (base::PathExists(work_dir) && base::DeletePathRecursively(work_dir)) {
      audit("work");
    }
    if (base::PathExists(records.AppendASCII(hash)) &&
        base::DeleteFile(records.AppendASCII(hash))) {
      audit("container_record");
    }
  }
}

}  // namespace

void RemoveProviderSandboxes(std::string_view profile_token) {
  base::FilePath local;
  if (base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
    SweepProviderSandboxes(local, profile_token, {}, /*explicit_removal=*/true,
                           nullptr);
  }
}

void ReconcileProviderSandboxes(const base::FilePath& local_app_data,
                                std::string_view profile_token,
                                const std::vector<std::string>& instances,
                                base::ListValue* diagnostics) {
  SweepProviderSandboxes(local_app_data, profile_token, instances,
                         /*explicit_removal=*/false, diagnostics);
}

bool RemoveAllProviderSandboxes(const base::FilePath& local_app_data) {
  {
    base::AutoLock lock(SandboxDirectoryLock());
    if (!ActiveSandboxes().empty()) {
      return LogSandboxFailure("sandbox_cleanup", "provider_still_live");
    }
  }
  auto root = local_app_data.Append(L"Krypton").Append(L"Agent");
  if (!base::PathExists(root)) {
    return true;
  }
  if (!PinLocalPath(root, /*directory=*/true)) {
    return false;
  }
  for (const auto* name : {L"containers", L"config", L"work", L"grants"}) {
    const auto folder = root.Append(name);
    if (base::IsLink(folder)) {
      return false;
    }
    base::FileEnumerator profiles(folder, /*recursive=*/false,
                                  base::FileEnumerator::DIRECTORIES);
    for (auto path = profiles.Next(); !path.empty(); path = profiles.Next()) {
      if (!base::IsLink(path)) {
        SweepProviderSandboxes(local_app_data, path.BaseName().MaybeAsASCII(),
                               {}, /*explicit_removal=*/true, nullptr);
      }
    }
  }
  base::FileEnumerator remaining(root.Append(L"grants"), /*recursive=*/true,
                                 base::FileEnumerator::FILES);
  base::FileEnumerator container_records(root.Append(L"containers"),
                                         /*recursive=*/false,
                                         base::FileEnumerator::DIRECTORIES);
  if (!remaining.Next().empty()) {
    return false;
  }
  for (auto path = container_records.Next(); !path.empty();
       path = container_records.Next()) {
    base::FileEnumerator entries(path, /*recursive=*/false,
                                 base::FileEnumerator::DIRECTORIES);
    if (!entries.Next().empty()) {
      return false;
    }
  }
  return true;
}

class LaunchAuthorization::State
    : public base::RefCountedThreadSafe<LaunchAuthorization::State> {
 public:
  State(base::Process supervisor,
        base::FilePath directory,
        ComponentPin pin,
        ProviderSandbox sandbox)
      : supervisor_(std::move(supervisor)),
        launcher_(directory.Append(L"krypton-agent-launcher.exe")),
        pin_(pin),
        sandbox_(std::move(sandbox)),
        relay_access_(sandbox_, directory.Append(L"krypton-agent.exe")),
        name_(base::StrCat({LR"(\\.\pipe\krypton-agent-launch-)",
                            base::ASCIIToWide(base::HexEncode(
                                base::RandBytesAsVector(16)))})) {}

  bool Open(std::string_view* failure) {
    const auto reject = [failure](std::string_view reason) {
      if (failure) {
        *failure = reason;
      }
      return LogSandboxFailure("launch_authorization_open", reason);
    };
    std::wstring user;
    if (!supervisor_.IsValid() || !stop_.is_valid() ||
        !base::win::GetUserSidString(&user)) {
      return reject("supervisor_identity");
    }
    const auto sddl = base::StrCat({L"D:P(A;;GA;;;", user, L")"});
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &raw_descriptor, nullptr)) {
      return reject("pipe_security_descriptor");
    }
    base::win::ScopedLocalAlloc descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),
                                   descriptor.get(), FALSE};
    pipe_.Set(CreateNamedPipeW(
        name_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE |
            FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS,
        1, 32768, 1, 0, &attributes));
    if (!pipe_.is_valid()) {
      return reject("launch_pipe");
    }
    if (!program_access_.Acquire()) {
      return reject("program_acl");
    }
    if (!relay_access_.Acquire()) {
      return reject("relay_acl");
    }
    if (!script_access_.Acquire()) {
      return reject("script_acl");
    }
    if (!package_access_.Acquire()) {
      return reject("package_acl");
    }
    return true;
  }

  base::DictValue Describe() const {
    auto description =
        base::DictValue()
            .Set("pipe", base::WideToUTF8(name_))
            .Set("program", sandbox_.program.AsUTF8Unsafe())
            .Set("cwd", sandbox_.working_directory.AsUTF8Unsafe())
            .Set("home",
                 (sandbox_.cli_directory.empty() ? sandbox_.config_directory
                                                 : sandbox_.cli_directory)
                     .AsUTF8Unsafe())
            .Set("owned_home", sandbox_.config_directory.AsUTF8Unsafe())
            .Set("provider_id", sandbox_.provider_id)
            .Set("sandbox_mode", IsolationName(sandbox_.isolation));
    if (!sandbox_.script.empty()) {
      description.Set("script", sandbox_.script.AsUTF8Unsafe());
    }
    if (!sandbox_.policy_directory.empty()) {
      description.Set("policy_dir", sandbox_.policy_directory.AsUTF8Unsafe());
    }
    return description;
  }

  void Stop() { SetEvent(stop_.get()); }

  void Serve() {
    base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
    auto payload = base::WriteJson(
        base::DictValue()
            .Set("program", sandbox_.program.AsUTF8Unsafe())
            .Set("cwd", sandbox_.working_directory.AsUTF8Unsafe())
            .Set("container", base::WideToUTF8(sandbox_.name))
            .Set("container_directory",
                 sandbox_.container_directory.AsUTF8Unsafe())
            .Set("owned_home", sandbox_.config_directory.AsUTF8Unsafe())
            .Set("script", sandbox_.script.AsUTF8Unsafe())
            .Set("isolation", IsolationName(sandbox_.isolation)));
    base::win::ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!payload || payload->size() > 32768 || !event.is_valid()) {
      return;
    }
    while (WaitForSingleObject(stop_.get(), 0) == WAIT_TIMEOUT &&
           WaitForSingleObject(supervisor_.Handle(), 0) == WAIT_TIMEOUT) {
      OVERLAPPED connection{};
      connection.hEvent = event.get();
      ResetEvent(event.get());
      DWORD transferred = 0;
      if (!ConnectNamedPipe(pipe_.get(), &connection)) {
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED &&
            (error != ERROR_IO_PENDING ||
             !FinishIo(connection, transferred, INFINITE))) {
          return;
        }
      }
      if (IsVerifiedLauncher()) {
        SendAndAcknowledge(*payload, event.get());
      }
      DisconnectNamedPipe(pipe_.get());
    }
  }

 private:
  friend class base::RefCountedThreadSafe<State>;
  ~State() = default;

  bool IsVerifiedLauncher() const {
    ULONG client_id = 0;
    if (!GetNamedPipeClientProcessId(pipe_.get(), &client_id)) {
      return false;
    }
    base::win::ScopedHandle client(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, client_id));
    if (!client.is_valid() ||
        base::GetParentProcessId(client.get()) != supervisor_.Pid()) {
      return false;
    }
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(client.get(), 0, image.data(), &size)) {
      return false;
    }
    image.resize(size);
    return base::FilePath::CompareEqualIgnoreCase(image, launcher_.value()) &&
           VerifyAgentLauncher(launcher_.DirName(), pin_) &&
           WaitForSingleObject(client.get(), 0) == WAIT_TIMEOUT &&
           WaitForSingleObject(supervisor_.Handle(), 0) == WAIT_TIMEOUT &&
           WaitForSingleObject(stop_.get(), 0) == WAIT_TIMEOUT;
  }

  bool FinishIo(OVERLAPPED& operation, DWORD& transferred, DWORD timeout) {
    const HANDLE handles[]{stop_.get(), supervisor_.Handle(), operation.hEvent};
    if (WaitForMultipleObjects(3, handles, FALSE, timeout) ==
        WAIT_OBJECT_0 + 2) {
      return GetOverlappedResult(pipe_.get(), &operation, &transferred, FALSE);
    }
    CancelIoEx(pipe_.get(), &operation);
    GetOverlappedResult(pipe_.get(), &operation, &transferred, TRUE);
    return false;
  }

  void SendAndAcknowledge(std::string_view payload, HANDLE event) {
    OVERLAPPED write{};
    write.hEvent = event;
    ResetEvent(event);
    DWORD transferred = 0;
    if (!WriteFile(pipe_.get(), payload.data(),
                   static_cast<DWORD>(payload.size()), &transferred, &write) &&
        (GetLastError() != ERROR_IO_PENDING ||
         !FinishIo(write, transferred, kResponseTimeoutMs))) {
      return;
    }
    if (transferred != payload.size()) {
      return;
    }
    // A disconnect discards unread data, so wait for the launcher to confirm.
    OVERLAPPED read{};
    read.hEvent = event;
    ResetEvent(event);
    char ack = 0;
    if (!ReadFile(pipe_.get(), &ack, sizeof(ack), &transferred, &read) &&
        (GetLastError() != ERROR_IO_PENDING ||
         !FinishIo(read, transferred, kResponseTimeoutMs))) {
      return;
    }
    if (transferred != sizeof(ack) || ack != kAcknowledgment) {
      Stop();
    }
  }

  const base::Process supervisor_;
  const base::FilePath launcher_;
  const ComponentPin pin_;
  const ProviderSandbox sandbox_;
  ProgramAccess program_access_{sandbox_, sandbox_.program};
  ProgramAccess relay_access_;
  ProgramAccess script_access_{sandbox_, sandbox_.script};
  ProgramAccess package_access_{sandbox_, sandbox_.package_root,
                                /*directory=*/true};
  const std::wstring name_;
  base::win::ScopedHandle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  base::win::ScopedHandle pipe_;
};

LaunchAuthorization::LaunchAuthorization(scoped_refptr<State> state)
    : state_(std::move(state)) {}

LaunchAuthorization::~LaunchAuthorization() {
  state_->Stop();
}

std::unique_ptr<LaunchAuthorization> LaunchAuthorization::Start(
    base::Process supervisor,
    const base::FilePath& component_directory,
    const ComponentPin& pin,
    ProviderSandbox sandbox,
    std::string_view* failure) {
  auto state = base::MakeRefCounted<State>(
      std::move(supervisor), component_directory, pin, std::move(sandbox));
  if (!state->Open(failure)) {
    LogSandboxFailure("launch_authorization_start", "open");
    return nullptr;
  }
  if (!base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                                  base::BindOnce(&State::Serve, state))) {
    if (failure) {
      *failure = "launch_worker";
    }
    LogSandboxFailure("launch_authorization_start", "launch_worker");
    return nullptr;
  }
  return std::unique_ptr<LaunchAuthorization>(
      new LaunchAuthorization(std::move(state)));
}

base::DictValue LaunchAuthorization::Describe() const {
  return state_->Describe();
}

}  // namespace krypton_agent
