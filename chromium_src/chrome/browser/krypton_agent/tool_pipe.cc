#include "chrome/browser/krypton_agent/tool_pipe.h"

#include <windows.h>

#include <sddl.h>

#include <array>
#include <deque>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base_paths_win.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_number_conversions_win.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/types/expected.h"
#include "base/win/access_token.h"
#include "base/win/scoped_handle.h"
#include "base/win/scoped_localalloc.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"
#include "crypto/secure_util.h"

namespace krypton_agent {
namespace {

constexpr size_t kMaxMessage = 4 * 1024 * 1024;
constexpr size_t kMaxQueuedReplies = 64;
constexpr DWORD kAuthTimeout = 10000;

class UserSecurity {
 public:
  bool Initialize(const base::win::Sid* package_sid = nullptr,
                  bool for_pipe = false) {
    HANDLE process_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
      return false;
    }
    base::win::ScopedHandle token(process_token);
    DWORD required = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
    if (!required) {
      return false;
    }
    std::vector<uint8_t> data(required);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), required,
                             &required)) {
      return false;
    }
    LPWSTR sid_string = nullptr;
    if (!ConvertSidToStringSidW(
            reinterpret_cast<const TOKEN_USER*>(data.data())->User.Sid,
            &sid_string)) {
      return false;
    }
    base::win::ScopedLocalAllocTyped<wchar_t> sid(sid_string);
    std::wstring sddl = base::StrCat({L"D:P(A;;GA;;;", sid.get(), L")"});
    if (package_sid) {
      const auto app_sddl = package_sid->ToSddlString();
      if (!app_sddl) {
        return false;
      }
      sddl += base::StrCat(
          {for_pipe ? L"(A;;GRGW;;;" : L"(A;;GR;;;", *app_sddl, L")"});
      if (for_pipe) {
        sddl += L"S:(ML;;NW;;;LW)";
      }
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
      return false;
    }
    descriptor_.reset(descriptor);
    attributes_.lpSecurityDescriptor = descriptor_.get();
    return true;
  }

  SECURITY_ATTRIBUTES* attributes() { return &attributes_; }

 private:
  base::win::ScopedLocalAlloc descriptor_;
  SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
};

bool EnsureDirectory(const base::FilePath& path, UserSecurity& security) {
  if (!CreateDirectoryW(path.value().c_str(), security.attributes()) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    return false;
  }
  const DWORD attributes = GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool PublishEndpoint(const base::FilePath& path,
                     std::string_view endpoint,
                     UserSecurity& security,
                     const std::wstring& nonce) {
  const base::FilePath temporary =
      path.DirName().Append(base::StrCat({L"pipe-", nonce, L".tmp"}));
  base::win::ScopedHandle file(CreateFileW(
      temporary.value().c_str(), GENERIC_WRITE, 0, security.attributes(),
      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.is_valid()) {
    return false;
  }
  DWORD written = 0;
  const bool complete =
      WriteFile(file.get(), endpoint.data(),
                static_cast<DWORD>(endpoint.size()), &written, nullptr) &&
      written == endpoint.size();
  file.Close();
  if (complete && MoveFileExW(temporary.value().c_str(), path.value().c_str(),
                              MOVEFILE_REPLACE_EXISTING)) {
    return true;
  }
  DeleteFileW(temporary.value().c_str());
  return false;
}

base::expected<base::FilePath, ToolPipe::ClientDecision> ClientImage(
    HANDLE pipe,
    const base::win::Sid* package_sid) {
  ULONG pid = 0;
  if (!GetNamedPipeClientProcessId(pipe, &pid)) {
    PLOG(WARNING) << "Krypton Agent could not identify the development client";
    return base::unexpected(ToolPipe::ClientDecision::kWrongImage);
  }
  base::win::ScopedHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (package_sid) {
    if (!process.is_valid()) {
      return base::unexpected(ToolPipe::ClientDecision::kWrongImage);
    }
    const auto token = base::win::AccessToken::FromProcess(process.get());
    if (!token || token->AppContainerSid() != *package_sid) {
      return base::unexpected(ToolPipe::ClientDecision::kWrongAppContainerSid);
    }
  }
  std::wstring image(32768, L'\0');
  DWORD size = static_cast<DWORD>(image.size());
  if (!process.is_valid() ||
      !QueryFullProcessImageNameW(process.get(), 0, image.data(), &size)) {
    return base::unexpected(ToolPipe::ClientDecision::kWrongImage);
  }
  image.resize(size);
  LOG(INFO) << "Krypton Agent pipe client PID " << pid;
  return base::FilePath(std::move(image));
}

base::DictValue Response(std::string_view id, base::DictValue result) {
  return base::DictValue()
      .Set("v", 1)
      .Set("type", "response")
      .Set("id", id)
      .Set("result", std::move(result));
}

}  // namespace

class ToolPipe::State : public base::RefCountedThreadSafe<State> {
 public:
  State(Handler handler,
        base::FilePath relay = {},
        std::optional<ComponentPin> pin = std::nullopt,
        Ready ready = {},
        std::optional<base::win::Sid> app_container = std::nullopt,
        ReportDecision report_decision = {})
      : client_sequence_(base::SequencedTaskRunner::GetCurrentDefault()),
        handler_(std::move(handler)),
        relay_(std::move(relay)),
        pin_(pin),
        app_container_(std::move(app_container)),
        ready_(std::move(ready)),
        report_decision_(std::move(report_decision)) {}

  void Stop() { SetEvent(stop_.get()); }

  void Run() {
    const bool opened = Open();
    if (ready_) {
      client_sequence_->PostTask(
          FROM_HERE, base::BindOnce(std::move(ready_),
                                    opened ? Endpoint{name_, endpoint_file_}
                                           : Endpoint{}));
    }
    if (!opened) {
      return;
    }
    while (!Stopped()) {
      if (!Connect()) {
        break;
      }
      const auto image =
          ClientImage(pipe_.get(), app_container_ ? &*app_container_ : nullptr);
      if (!pin_) {
        Serve();
      } else if (!image) {
        Report(image.error());
      } else if (!base::FilePath::CompareEqualIgnoreCase(image->value(),
                                                         relay_.value())) {
        Report(ClientDecision::kWrongImage);
      } else if (!VerifyAgentImage(*image, *pin_)) {
        Report(ClientDecision::kFailedSignatureCheck);
      } else {
        if (!Serve() && !Stopped()) {
          Report(ClientDecision::kBadToken);
        }
      }
      DisconnectNamedPipe(pipe_.get());
      base::AutoLock lock(replies_lock_);
      ++connection_;
      replies_.clear();
      ResetEvent(reply_event_.get());
    }
    if (!endpoint_file_.empty()) {
      std::string previous;
      if (base::ReadFileToStringWithMaxSize(endpoint_file_, &previous, 4096) &&
          previous == endpoint_) {
        base::DeleteFile(endpoint_file_);
      }
    }
  }

  bool valid() const { return stop_.is_valid() && reply_event_.is_valid(); }

 private:
  friend class base::RefCountedThreadSafe<State>;
  ~State() = default;

  void Report(ClientDecision decision) {
    if (report_decision_) {
      client_sequence_->PostTask(FROM_HERE,
                                 base::BindOnce(report_decision_, decision));
    }
  }

  bool Stopped() const {
    return WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0;
  }

  bool Open() {
    UserSecurity security;
    UserSecurity pipe_security;
    UserSecurity token_security;
    const auto* package_sid = app_container_ ? &*app_container_ : nullptr;
    base::FilePath local;
    if (!security.Initialize() ||
        !pipe_security.Initialize(package_sid, true) ||
        !token_security.Initialize(package_sid) ||
        !base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
      return false;
    }
    if (!PrepareAgentPrivateDirectories(local)) {
      return false;
    }
    const base::FilePath krypton = local.Append(L"Krypton");
    const base::FilePath agent = krypton.Append(L"Agent");
    const base::FilePath dev = agent.Append(pin_ ? L"tokens" : L"dev");
    std::array<uint8_t, 16> nonce;
    base::RandBytes(nonce);
    base::RandBytes(token_);
    const std::string session_id = base::HexEncode(nonce);
    const auto random = base::UTF8ToWide(session_id);
    const std::wstring suffix = base::StrCat({L"krypton-agent-", random});
    std::wstring name = base::StrCat({LR"(\\.\pipe\)", suffix});
    std::wstring client_name = name;
    if (package_sid) {
      DWORD windows_session = 0;
      const auto sid = package_sid->ToSddlString();
      if (!sid ||
          !ProcessIdToSessionId(GetCurrentProcessId(), &windows_session)) {
        return false;
      }
      name = base::StrCat(
          {LR"(\\.\pipe\Sessions\)", base::NumberToWString(windows_session),
           LR"(\AppContainerNamedObjects\)", *sid, L"\\", suffix});
      client_name = base::StrCat({LR"(\\.\pipe\LOCAL\)", suffix});
    }
    pipe_.Set(CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1,
        65536, 65536, 0, pipe_security.attributes()));
    if (!pipe_.is_valid() || Stopped() || !EnsureDirectory(krypton, security) ||
        !EnsureDirectory(agent, security) || !EnsureDirectory(dev, security)) {
      LOG(ERROR) << "Krypton Agent could not open a development pipe";
      return false;
    }
    session_id_ = session_id;
    name_ = base::WideToUTF8(client_name);
    endpoint_file_ =
        dev.Append(pin_ ? base::StrCat({random, L".json"}) : L"pipe.json");
    endpoint_ = base::WriteJson(base::DictValue()
                                    .Set("pipe", name_)
                                    .Set("token", base::HexEncode(token_)))
                    .value_or("");
    if (!PublishEndpoint(endpoint_file_, endpoint_, token_security, random)) {
      LOG(ERROR) << "Krypton Agent could not write development pipe metadata";
      return false;
    }
    LOG(INFO) << "Krypton Agent development pipe ready";
    return true;
  }

  bool FinishIo(OVERLAPPED& operation, DWORD& bytes, DWORD timeout) {
    const HANDLE handles[]{stop_.get(), operation.hEvent};
    if (WaitForMultipleObjects(2, handles, FALSE, timeout) ==
        WAIT_OBJECT_0 + 1) {
      return GetOverlappedResult(pipe_.get(), &operation, &bytes, FALSE);
    }
    CancelIoEx(pipe_.get(), &operation);
    GetOverlappedResult(pipe_.get(), &operation, &bytes, TRUE);
    return false;
  }

  bool Connect() {
    base::win::ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.is_valid()) {
      return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe_.get(), &operation)) {
      return true;
    }
    const DWORD error = GetLastError();
    DWORD ignored = 0;
    return error == ERROR_PIPE_CONNECTED ||
           (error == ERROR_IO_PENDING &&
            FinishIo(operation, ignored, INFINITE));
  }

  bool Write(std::string_view message) {
    base::win::ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.is_valid()) {
      return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD written = 0;
    return (WriteFile(pipe_.get(), message.data(),
                      static_cast<DWORD>(message.size()), &written,
                      &operation) ||
            (GetLastError() == ERROR_IO_PENDING &&
             FinishIo(operation, written, kAuthTimeout))) &&
           written == message.size();
  }

  bool ReceiveLine(std::string line, bool& authenticated) {
    auto message = base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
    if (!message || message->FindInt("v") != 1) {
      return false;
    }
    const auto* type = message->FindString("type");
    const auto* method = message->FindString("method");
    const auto* id = message->FindString("id");
    if (!type || *type != "request" || !method || !id || id->empty() ||
        id->size() > 128 || !id->starts_with("s-")) {
      return false;
    }
    if (!authenticated) {
      const auto* params = message->FindDict("params");
      const auto* token = params ? params->FindString("token") : nullptr;
      std::array<uint8_t, 32> decoded;
      if (*method != "auth" || !token ||
          !base::HexStringToSpan(*token, decoded) ||
          !crypto::SecureMemEqual(token_, decoded)) {
        return false;
      }
      authenticated = Write(
          base::WriteJson(Response(*id, base::DictValue()
                                            .Set("session_id", session_id_)
                                            .Set("protocol_version", 1)))
              .value_or("") +
          "\n");
      if (authenticated) {
        Report(ClientDecision::kAccepted);
      }
      return authenticated;
    }
    if (*method == "auth") {
      return false;
    }
    uint64_t connection;
    {
      base::AutoLock lock(replies_lock_);
      connection = connection_;
    }
    auto reply = base::BindOnce(&State::QueueReply, base::WrapRefCounted(this),
                                connection);
    return client_sequence_->PostTask(
        FROM_HERE,
        base::BindOnce(handler_, std::move(*message), std::move(reply)));
  }

  void QueueReply(uint64_t connection, base::DictValue response) {
    auto serialized = base::WriteJson(response);
    if (!serialized || serialized->size() >= kMaxMessage) {
      return;
    }
    serialized->push_back('\n');
    base::AutoLock lock(replies_lock_);
    if (connection != connection_) {
      return;
    }
    if (replies_.size() >= kMaxQueuedReplies) {
      Stop();
      return;
    }
    replies_.push_back(std::move(*serialized));
    SetEvent(reply_event_.get());
  }

  bool SendReplies() {
    std::deque<std::string> pending;
    {
      base::AutoLock lock(replies_lock_);
      pending.swap(replies_);
      ResetEvent(reply_event_.get());
    }
    for (const auto& reply : pending) {
      if (!Write(reply)) {
        return false;
      }
    }
    return true;
  }

  bool Serve() {
    base::win::ScopedHandle read_event(
        CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!read_event.is_valid()) {
      return false;
    }
    std::array<char, 8192> buffer;
    std::string input;
    bool authenticated = false;
    while (!Stopped()) {
      ResetEvent(read_event.get());
      OVERLAPPED operation{};
      operation.hEvent = read_event.get();
      DWORD bytes = 0;
      if (!ReadFile(pipe_.get(), buffer.data(), buffer.size(), &bytes,
                    &operation)) {
        if (GetLastError() != ERROR_IO_PENDING) {
          return authenticated;
        }
        const HANDLE handles[]{stop_.get(), read_event.get(),
                               reply_event_.get()};
        DWORD wait = 0;
        do {
          wait = WaitForMultipleObjects(
              3, handles, FALSE, authenticated ? INFINITE : kAuthTimeout);
        } while (wait == WAIT_OBJECT_0 + 2 && SendReplies());
        if (wait != WAIT_OBJECT_0 + 1) {
          CancelIoEx(pipe_.get(), &operation);
          GetOverlappedResult(pipe_.get(), &operation, &bytes, TRUE);
          return authenticated;
        }
        if (!GetOverlappedResult(pipe_.get(), &operation, &bytes, FALSE)) {
          return authenticated;
        }
      }
      if (!bytes) {
        return authenticated;
      }
      input.append(buffer.data(), bytes);
      size_t start = 0;
      for (size_t end = input.find('\n'); end != std::string::npos;
           end = input.find('\n', start)) {
        if (end - start > kMaxMessage ||
            !ReceiveLine(input.substr(start, end - start), authenticated)) {
          return authenticated;
        }
        start = end + 1;
      }
      input.erase(0, start);
      if (input.size() > kMaxMessage || !SendReplies()) {
        return authenticated;
      }
    }
    return authenticated;
  }

  const scoped_refptr<base::SequencedTaskRunner> client_sequence_;
  const Handler handler_;
  const base::FilePath relay_;
  const std::optional<ComponentPin> pin_;
  const std::optional<base::win::Sid> app_container_;
  Ready ready_;
  const ReportDecision report_decision_;
  std::string name_;
  const base::win::ScopedHandle stop_{
      CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  const base::win::ScopedHandle reply_event_{
      CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  base::win::ScopedHandle pipe_;
  base::FilePath endpoint_file_;
  std::string endpoint_;
  std::string session_id_;
  std::array<uint8_t, 32> token_{};
  base::Lock replies_lock_;
  std::deque<std::string> replies_ GUARDED_BY(replies_lock_);
  uint64_t connection_ GUARDED_BY(replies_lock_) = 0;
};

ToolPipe::ToolPipe(scoped_refptr<State> state) : state_(std::move(state)) {}

ToolPipe::~ToolPipe() {
  state_->Stop();
}

std::unique_ptr<ToolPipe> ToolPipe::Start(Handler handler) {
  auto state = base::MakeRefCounted<State>(std::move(handler));
  if (!state->valid() ||
      !base::ThreadPool::PostTask(
          FROM_HERE,
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
          base::BindOnce(&State::Run, state))) {
    return {};
  }
  return std::unique_ptr<ToolPipe>(new ToolPipe(std::move(state)));
}

std::unique_ptr<ToolPipe> ToolPipe::StartVerified(
    Handler handler,
    const base::FilePath& relay,
    const ComponentPin& pin,
    const std::optional<base::win::Sid>& app_container,
    Ready ready,
    ReportDecision report_decision,
    bool job_only) {
  if ((app_container && job_only) ||
      (!app_container && !job_only && !pin.unsigned_development)) {
    return nullptr;
  }
  auto state = base::MakeRefCounted<State>(
      std::move(handler), relay, pin, std::move(ready),
      app_container ? std::make_optional(app_container->Clone()) : std::nullopt,
      std::move(report_decision));
  if (!state->valid() ||
      !base::ThreadPool::PostTask(
          FROM_HERE,
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
          base::BindOnce(&State::Run, state))) {
    return nullptr;
  }
  return std::unique_ptr<ToolPipe>(new ToolPipe(std::move(state)));
}

}  // namespace krypton_agent
