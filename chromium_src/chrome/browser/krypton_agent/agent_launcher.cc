#include <windows.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "base/win/access_token.h"
#include "base/win/scoped_handle.h"
#include "base/win/startup_information.h"
#include "base/win/trust_util.h"
#include "sandbox/win/src/app_container_base.h"
#include "sandbox/win/src/security_capabilities.h"

namespace {

constexpr std::wstring_view kAuthorizationPrefix =
    LR"(\\.\pipe\krypton-agent-launch-)";
constexpr std::wstring_view kContainerPrefix = L"Krypton.Agent.";

#if defined(KRYPTON_AGENT_DEVELOPMENT_LAUNCHER)
constexpr wchar_t kDevelopmentDirectory[] = L"krypton-agent-dev-component";
bool IsLocalPath(const base::FilePath& path);
#endif

base::FilePath ProcessImage(HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
    return {};
  }
  path.resize(length);
  return base::FilePath(path);
}

bool TrustedBroker(HANDLE pipe, const base::FilePath& self) {
  ULONG server = 0;
  if (!GetNamedPipeServerProcessId(pipe, &server)) {
    return false;
  }
  base::win::ScopedHandle supervisor(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                  base::GetParentProcessId(GetCurrentProcess())));
  if (!supervisor.is_valid() ||
      server != base::GetParentProcessId(supervisor.get())) {
    return false;
  }
  base::win::ScopedHandle broker(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server));
  if (!broker.is_valid()) {
    return false;
  }
  const auto supervisor_image = ProcessImage(supervisor.get());
  const auto broker_image = ProcessImage(broker.get());
  const auto directory = self.DirName();
  if (!base::FilePath::CompareEqualIgnoreCase(
          supervisor_image.value(),
          directory.Append(L"krypton-agent.exe").value()) ||
      broker_image.empty()) {
    return false;
  }
#if defined(KRYPTON_AGENT_DEVELOPMENT_LAUNCHER)
  return IsLocalPath(self) &&
         base::FilePath::CompareEqualIgnoreCase(directory.BaseName().value(),
                                                kDevelopmentDirectory) &&
         base::FilePath::CompareEqualIgnoreCase(
             broker_image.value(),
             directory.DirName().Append(L"chrome.exe").value());
#else
  return base::FilePath::CompareEqualIgnoreCase(broker_image.BaseName().value(),
                                                L"chrome.exe") &&
         base::win::IsBinaryTrusted(supervisor_image, /*verify_publisher=*/true,
                                    /*force_verify_in_dev_builds=*/true) &&
         base::win::IsBinaryTrusted(broker_image, /*verify_publisher=*/true,
                                    /*force_verify_in_dev_builds=*/true);
#endif
}

std::optional<base::DictValue> ReadAuthorization(const std::wstring& endpoint,
                                                 const base::FilePath& self) {
  if (!std::wstring_view(endpoint).starts_with(kAuthorizationPrefix) ||
      endpoint.size() != kAuthorizationPrefix.size() + 32 ||
      !std::ranges::all_of(
          std::wstring_view(endpoint).substr(kAuthorizationPrefix.size()),
          [](wchar_t ch) { return base::IsHexDigit(ch); })) {
    return std::nullopt;
  }
  base::win::ScopedHandle pipe;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  while (!pipe.is_valid()) {
    pipe.Set(CreateFileW(endpoint.c_str(),
                         GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                         0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                         nullptr));
    if (pipe.is_valid()) {
      break;
    }
    const DWORD error = GetLastError();
    const auto now = GetTickCount64();
    if (error != ERROR_PIPE_BUSY || now >= deadline ||
        !WaitNamedPipeW(endpoint.c_str(), static_cast<DWORD>(deadline - now))) {
      return std::nullopt;
    }
  }
  if (!pipe.is_valid() || !TrustedBroker(pipe.get(), self)) {
    return std::nullopt;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
    return std::nullopt;
  }
  base::win::ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.is_valid()) {
    return std::nullopt;
  }
  std::array<char, 32768> bytes;
  OVERLAPPED read{};
  read.hEvent = event.get();
  DWORD length = 0;
  if (!ReadFile(pipe.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                &length, &read)) {
    if (GetLastError() != ERROR_IO_PENDING) {
      return std::nullopt;
    }
    if (WaitForSingleObject(event.get(), 5000) != WAIT_OBJECT_0) {
      CancelIoEx(pipe.get(), &read);
      GetOverlappedResult(pipe.get(), &read, &length, TRUE);
      return std::nullopt;
    }
    if (!GetOverlappedResult(pipe.get(), &read, &length, FALSE)) {
      return std::nullopt;
    }
  }
  auto authorization = base::JSONReader::ReadDict(
      std::string_view(bytes.data(), length), base::JSON_PARSE_RFC);
  if (!authorization) {
    return std::nullopt;
  }
  ResetEvent(event.get());
  OVERLAPPED write{};
  write.hEvent = event.get();
  constexpr char ack = 'K';
  if (!WriteFile(pipe.get(), &ack, sizeof(ack), &length, &write)) {
    if (GetLastError() != ERROR_IO_PENDING) {
      return std::nullopt;
    }
    if (WaitForSingleObject(event.get(), 5000) != WAIT_OBJECT_0) {
      CancelIoEx(pipe.get(), &write);
      GetOverlappedResult(pipe.get(), &write, &length, TRUE);
      return std::nullopt;
    }
    if (!GetOverlappedResult(pipe.get(), &write, &length, FALSE)) {
      return std::nullopt;
    }
  }
  return length == sizeof(ack) ? std::move(authorization) : std::nullopt;
}

bool ValidContainerName(std::wstring_view name) {
  if (!name.starts_with(kContainerPrefix)) {
    return false;
  }
  name.remove_prefix(kContainerPrefix.size());
  return name.size() > 33 && name.size() <= 97 && name[32] == L'.' &&
         std::ranges::all_of(name.substr(0, 32),
                             [](wchar_t ch) { return base::IsHexDigit(ch); }) &&
         std::ranges::all_of(name.substr(33), [](wchar_t ch) {
           return base::IsAsciiAlpha(ch) || base::IsAsciiDigit(ch) ||
                  ch == L'-' || ch == L'_';
         });
}

bool IsLocalPath(const base::FilePath& path) {
  const auto& value = path.value();
  return value.size() >= 4 && base::IsAsciiAlpha(value[0]) &&
         value[1] == L':' && value[2] == L'\\' && !path.ReferencesParent() &&
         GetDriveTypeW(value.substr(0, 3).c_str()) == DRIVE_FIXED;
}

struct EnvironmentOrder {
  bool operator()(const std::wstring& left, const std::wstring& right) const {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
           CSTR_LESS_THAN;
  }
};

std::optional<std::wstring> BuildEnvironment(const base::FilePath& container,
                                             const base::FilePath& home) {
  const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> variables(
      GetEnvironmentStringsW(), &FreeEnvironmentStringsW);
  if (!variables) {
    return std::nullopt;
  }
  std::map<std::wstring, std::wstring, EnvironmentOrder> environment;
  for (const wchar_t* cursor = variables.get(); *cursor;) {
    const std::wstring_view entry(cursor);
    cursor += entry.size() + 1;
    const size_t equal = entry.find(L'=', 1);
    if (equal == entry.npos) {
      continue;
    }
    const auto key = entry.substr(0, equal);
    std::wstring upper(key);
    std::ranges::transform(upper, upper.begin(),
                           [](wchar_t ch) { return base::ToUpperASCII(ch); });
    if (upper == L"NODE_OPTIONS" || upper == L"COMSPEC" ||
        upper == L"__COMPAT_LAYER" || upper == L"CLAUDE_CODE_TMPDIR" ||
        upper.starts_with(L"BUN_") || upper.starts_with(L"ELECTRON_") ||
        upper.starts_with(L"COR_") || upper.starts_with(L"CORECLR_")) {
      continue;
    }
    environment.emplace(key, entry.substr(equal + 1));
  }
  if (!container.empty()) {
    const auto temp = container.Append(L"Temp");
    for (const auto* key : {L"TEMP", L"TMP", L"TMPDIR"}) {
      environment.insert_or_assign(key, temp.value());
    }
    environment.insert_or_assign(L"LOCALAPPDATA", container.value());
    environment.insert_or_assign(L"HOME", home.value());
    environment.insert_or_assign(L"USERPROFILE", home.value());
    environment.insert_or_assign(
        L"APPDATA", home.Append(L"AppData").Append(L"Roaming").value());
  }
  std::wstring block;
  for (const auto& [key, value] : environment) {
    block.append(key).push_back(L'=');
    block.append(value).push_back(L'\0');
  }
  if (block.empty()) {
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

bool VerifyChildToken(HANDLE child,
                      const sandbox::AppContainerBase* container,
                      bool lpac) {
  const auto token = base::win::AccessToken::FromProcess(child);
  if (!token) {
    return false;
  }
  if (!container) {
    return !token->IsAppContainer();
  }
  const auto capabilities = token->Capabilities();
  DWORD is_lpac = 0;
  DWORD bytes = 0;
  if (lpac &&
      (!GetTokenInformation(token->get(), TokenIsLessPrivilegedAppContainer,
                            &is_lpac, sizeof(is_lpac), &bytes) ||
       !is_lpac)) {
    return false;
  }
  return token->IsAppContainer() &&
         token->AppContainerSid() == container->GetPackageSid() &&
         token->IntegrityLevel() == SECURITY_MANDATORY_LOW_RID &&
         capabilities.size() == 1 &&
         capabilities.front().GetSid() ==
             base::win::Sid::FromKnownCapability(
                 base::win::WellKnownCapability::kInternetClient);
}

int Run(const base::DictValue& launch,
        base::span<const std::wstring> arguments) {
  const auto* name = launch.FindString("container");
  const auto* program = launch.FindString("program");
  const auto* working_directory = launch.FindString("cwd");
  const auto* mode = launch.FindString("isolation");
  const auto* owned_home = launch.FindString("owned_home");
  const auto* local_app_data = launch.FindString("container_directory");
  const auto* script = launch.FindString("script");
  if (!name || !program || !working_directory || !mode || !owned_home ||
      !local_app_data || !script || name->find('\0') != std::string::npos ||
      program->find('\0') != std::string::npos ||
      working_directory->find('\0') != std::string::npos ||
      owned_home->find('\0') != std::string::npos ||
      local_app_data->find('\0') != std::string::npos ||
      script->find('\0') != std::string::npos) {
    return ERROR_INVALID_PARAMETER;
  }
  const auto container_name = base::UTF8ToWide(*name);
  const auto image = base::FilePath::FromUTF8Unsafe(*program);
  const auto cwd = base::FilePath::FromUTF8Unsafe(*working_directory);
  const auto home = base::FilePath::FromUTF8Unsafe(*owned_home);
  const auto container_path = base::FilePath::FromUTF8Unsafe(*local_app_data);
  const bool job_only = *mode == "job";
  const bool lpac = *mode == "lpac";
  if ((!job_only && !lpac && *mode != "app_container") ||
      (!job_only &&
       (!ValidContainerName(container_name) || !IsLocalPath(container_path))) ||
      (job_only && (!name->empty() || !local_app_data->empty())) ||
      !IsLocalPath(image) || !IsLocalPath(cwd) || !IsLocalPath(home) ||
      (!script->empty() &&
       (arguments.empty() ||
        !base::FilePath::CompareEqualIgnoreCase(arguments.front(),
                                                base::UTF8ToWide(*script)))) ||
      !base::FilePath::CompareEqualIgnoreCase(image.Extension(), L".exe")) {
    return ERROR_INVALID_PARAMETER;
  }

  std::wstring child_command =
      base::CommandLine::QuoteForCommandLineToArgvW(image.value());
  for (const auto& argument : arguments) {
    child_command.push_back(L' ');
    if (argument.empty()) {
      child_command.append(L"\"\"");
    } else if (argument.size() == 2 && argument[0] == L'%') {
      // This command line goes directly to CreateProcessW.
      child_command.append(argument);
    } else {
      child_command.append(
          base::CommandLine::QuoteForCommandLineToArgvW(argument));
    }
  }
  if (child_command.size() >= 32767) {
    return ERROR_INVALID_PARAMETER;
  }

  auto environment = BuildEnvironment(container_path, home);
  if (!environment) {
    return ERROR_ACCESS_DENIED;
  }
  auto container =
      job_only ? nullptr : sandbox::AppContainerBase::Open(container_name);
  if (!job_only && !container) {
    return ERROR_ACCESS_DENIED;
  }
  std::unique_ptr<sandbox::SecurityCapabilities> capabilities;
  if (container) {
    container->AddCapability(base::win::WellKnownCapability::kInternetClient);
    container->SetEnableLowPrivilegeAppContainer(lpac);
    capabilities = container->GetSecurityCapabilities();
  }

  base::win::ScopedHandle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.is_valid() ||
      !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    return ERROR_ACCESS_DENIED;
  }

  constexpr std::array<DWORD, 3> streams{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE,
                                         STD_ERROR_HANDLE};
  std::array<base::win::ScopedHandle, 3> duplicates;
  std::array<HANDLE, 3> inherited{};
  const HANDLE current = GetCurrentProcess();
  for (size_t index = 0; index < streams.size(); ++index) {
    const HANDLE source = GetStdHandle(streams[index]);
    const DWORD type = GetFileType(source);
    if (type != FILE_TYPE_PIPE && (index != 2 || type != FILE_TYPE_CHAR)) {
      return ERROR_INVALID_HANDLE;
    }
    if (!DuplicateHandle(current, source, current, &inherited[index], 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
      return ERROR_INVALID_HANDLE;
    }
    duplicates[index].Set(inherited[index]);
  }

  base::win::StartupInformation startup;
  HANDLE job_handle = job.get();
  DWORD package_policy = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
  if (!startup.InitializeProcThreadAttributeList(job_only ? 2
                                                 : lpac   ? 4
                                                          : 3) ||
      (capabilities &&
       !startup.UpdateProcThreadAttribute(
           PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, capabilities.get(),
           sizeof(SECURITY_CAPABILITIES))) ||
      !startup.UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         inherited.data(), sizeof(inherited)) ||
      !startup.UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                         &job_handle, sizeof(job_handle)) ||
      (lpac && !startup.UpdateProcThreadAttribute(
                   PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
                   &package_policy, sizeof(package_policy)))) {
    return ERROR_ACCESS_DENIED;
  }
  auto* info = startup.startup_info();
  info->dwFlags = STARTF_USESTDHANDLES;
  info->hStdInput = inherited[0];
  info->hStdOutput = inherited[1];
  info->hStdError = inherited[2];

  PROCESS_INFORMATION process{};
  if (!CreateProcessW(
          image.value().c_str(), child_command.data(), nullptr, nullptr, TRUE,
          CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
              CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
          environment->data(), cwd.value().c_str(), info, &process)) {
    return static_cast<int>(GetLastError());
  }
  for (auto& handle : duplicates) {
    handle.Close();
  }
  base::win::ScopedHandle child(process.hProcess);
  base::win::ScopedHandle thread(process.hThread);
  if (!VerifyChildToken(child.get(), container.get(), lpac) ||
      ResumeThread(thread.get()) != 1) {
    TerminateProcess(child.get(), ERROR_ACCESS_DENIED);
    return ERROR_ACCESS_DENIED;
  }
  DWORD exit_code = ERROR_PROCESS_ABORTED;
  if (WaitForSingleObject(child.get(), INFINITE) == WAIT_OBJECT_0) {
    GetExitCodeProcess(child.get(), &exit_code);
  }
  return static_cast<int>(exit_code);
}

}  // namespace

int main() {
  base::CommandLine::Init(0, nullptr);
  const auto& command = *base::CommandLine::ForCurrentProcess();
  const auto arguments = command.GetArgs();
  if (arguments.empty() || !command.GetSwitches().empty() ||
      command.argv().size() < 3 || command.argv()[2] != L"--") {
    return ERROR_INVALID_PARAMETER;
  }
  base::FilePath self;
  if (!base::PathService::Get(base::FILE_EXE, &self)) {
    return ERROR_ACCESS_DENIED;
  }
  const auto launch = ReadAuthorization(arguments[0], self);
  return launch ? Run(*launch, base::span(arguments).subspan<1>())
                : ERROR_ACCESS_DENIED;
}
