#include "chrome/browser/krypton_agent/sidecar_process.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <utility>

#include "base/base_paths_win.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/process.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/win/scoped_handle.h"
#include "base/win/startup_information.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"

namespace krypton_agent {
namespace {

constexpr size_t kMaxFrame = 4 * 1024 * 1024;
constexpr int kMaxPending = 16;

bool CreatePipePair(base::win::ScopedHandle& reader,
                    base::win::ScopedHandle& writer,
                    bool child_reads) {
  HANDLE inbound = nullptr;
  HANDLE outbound = nullptr;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  if (!CreatePipe(&inbound, &outbound, &attributes, 0)) {
    return false;
  }
  reader.Set(inbound);
  writer.Set(outbound);
  return SetHandleInformation(child_reads ? outbound : inbound,
                              HANDLE_FLAG_INHERIT, 0);
}
}  // namespace

class SidecarProcess::State : public base::RefCountedThreadSafe<State> {
 public:
  State(Message message, Closed closed, Ready ready)
      : main_(base::SequencedTaskRunner::GetCurrentDefault()),
        writer_(
            base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()})),
        message_(std::move(message)),
        closed_(std::move(closed)),
        on_ready_(std::move(ready)) {}

  void Stop() {
    stopped_.store(true);
    base::AutoLock guard(mutex_);
    if (job_.is_valid()) {
      TerminateJobObject(job_.get(), ERROR_CANCELLED);
    }
  }

  void Run(base::FilePath image,
           ComponentPin pin,
           std::optional<ProviderSandbox> sandbox) {
    auto process = Launch(image, pin);
    if (process.IsValid()) {
      std::unique_ptr<LaunchAuthorization> authorization;
      if (sandbox) {
        authorization = LaunchAuthorization::Start(
            process.Duplicate(), image.DirName(), pin, std::move(*sandbox));
      }
      if ((!sandbox || authorization) &&
          main_->PostTask(
              FROM_HERE,
              base::BindOnce(std::move(on_ready_),
                             authorization ? authorization->Describe()
                                           : base::DictValue()))) {
        Read();
      }
      Stop();
    }
    main_->PostTask(FROM_HERE, std::move(closed_));
  }

  bool Send(base::DictValue message) {
    if (stopped_.load() || !ready_.load()) {
      return false;
    }
    if (pending_.fetch_add(1) >= kMaxPending) {
      pending_.fetch_sub(1);
      return false;
    }
    auto frame = base::WriteJson(message);
    if (!frame || frame->size() >= kMaxFrame) {
      pending_.fetch_sub(1);
      return false;
    }
    frame->push_back('\n');
    if (!writer_->PostTask(
            FROM_HERE, base::BindOnce(&State::Write, base::WrapRefCounted(this),
                                      std::move(*frame)))) {
      pending_.fetch_sub(1);
      return false;
    }
    return true;
  }

  bool AuthorizeProviders(std::string profile_token,
                          std::vector<std::string> instances,
                          std::vector<ProviderLaunchRequest> requests,
                          Ready done) {
    return base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&State::PrepareProviders, base::WrapRefCounted(this),
                       std::move(profile_token), std::move(instances),
                       std::move(requests), std::move(done)));
  }

 private:
  friend class base::RefCountedThreadSafe<State>;
  ~State() = default;

  void PrepareProviders(std::string profile_token,
                        std::vector<std::string> instances,
                        std::vector<ProviderLaunchRequest> requests,
                        Ready done) {
    base::DictValue descriptions;
    base::ListValue diagnostics;
    std::vector<std::unique_ptr<LaunchAuthorization>> authorizations;
    base::Process supervisor;
    base::FilePath directory;
    std::optional<ComponentPin> pin;
    {
      base::AutoLock guard(mutex_);
      if (!stopped_.load()) {
        supervisor = supervisor_.Duplicate();
        directory = directory_;
        pin = pin_;
      }
    }
    if (supervisor.IsValid() && pin) {
      auto* audit = pin->unsigned_development ? &diagnostics : nullptr;
      for (auto& request : requests) {
        if (stopped_.load()) {
          break;
        }
        const auto reject = [&request, audit](std::string_view reason) {
          LOG(WARNING) << "Krypton Agent provider authorization failed ["
                       << reason << "] for " << request.instance;
          if (audit) {
            audit->Append(base::DictValue()
                              .Set("event", "authorization_failed")
                              .Set("instance_id", request.instance)
                              .Set("reason", reason));
          }
        };
        std::string_view failure;
        auto sandbox = PrepareProviderSandbox(std::move(request.program),
                                              profile_token, request.instance,
                                              request.isolation, &failure);
        if (!sandbox) {
          reject(failure);
          continue;
        }
        auto authorization =
            LaunchAuthorization::Start(supervisor.Duplicate(), directory, *pin,
                                       std::move(*sandbox), &failure);
        if (!authorization) {
          reject(failure);
          continue;
        }
        auto description = authorization->Describe();
        if (request.isolation == ProviderIsolation::kJob &&
            IsLocalProviderUsageDatabase(request.usage_database_path)) {
          description.Set("usage_database_path",
                          std::move(request.usage_database_path));
        }
        descriptions.Set(request.instance, std::move(description));
        authorizations.push_back(std::move(authorization));
      }
      base::FilePath local;
      // A partial or failed authorization must never drive data cleanup.
      if (!stopped_.load() && !requests.empty() &&
          authorizations.size() == requests.size() &&
          base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
        ReconcileProviderSandboxes(local, profile_token, instances, audit);
      }
    } else {
      LOG(WARNING) << "Krypton Agent provider authorization failed "
                      "[panel_supervisor]";
    }
    {
      base::AutoLock guard(mutex_);
      if (!stopped_.load()) {
        panel_authorizations_.swap(authorizations);
      }
    }
    auto params =
        base::DictValue().Set("authorizations", std::move(descriptions));
    if (!diagnostics.empty()) {
      params.Set("host_diagnostics", std::move(diagnostics));
    }
    main_->PostTask(FROM_HERE,
                    base::BindOnce(std::move(done), std::move(params)));
  }

  base::Process Launch(const base::FilePath& image, const ComponentPin& pin) {
    base::win::ScopedHandle child_in;
    base::win::ScopedHandle child_out;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    base::win::ScopedHandle child_error(CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (stopped_.load() || !child_error.is_valid() ||
        !VerifyAgentImage(image, pin) ||
        !VerifyAgentLauncher(image.DirName(), pin) ||
        !CreatePipePair(child_in, output_, true) ||
        !CreatePipePair(input_, child_out, false)) {
      return base::Process();
    }
    base::win::ScopedHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.is_valid() ||
        !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
      return base::Process();
    }
    base::win::StartupInformation startup;
    std::array<HANDLE, 3> handles{child_in.get(), child_out.get(),
                                  child_error.get()};
    HANDLE job_handle = job.get();
    DWORD64 mitigations =
        PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_FONT_DISABLE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_PREFER_SYSTEM32_ALWAYS_ON;
    if (!startup.InitializeProcThreadAttributeList(3) ||
        !startup.UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           handles.data(), sizeof(handles)) ||
        !startup.UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                           &job_handle, sizeof(job_handle)) ||
        !startup.UpdateProcThreadAttribute(
            PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations,
            sizeof(mitigations))) {
      return base::Process();
    }
    auto* info = startup.startup_info();
    info->dwFlags = STARTF_USESTDHANDLES;
    info->hStdInput = child_in.get();
    info->hStdOutput = child_out.get();
    info->hStdError = child_error.get();
    std::wstring command = base::CommandLine(image).GetCommandLineString();
    const auto directory = image.DirName();
    PROCESS_INFORMATION created{};
    base::AutoLock guard(mutex_);
    if (stopped_.load() ||
        !CreateProcessW(image.value().c_str(), command.data(), nullptr, nullptr,
                        TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        nullptr, directory.value().c_str(), info, &created)) {
      return base::Process();
    }
    base::win::ScopedHandle thread(created.hThread);
    base::Process process(created.hProcess);
    job_ = std::move(job);
    supervisor_ = process.Duplicate();
    directory_ = directory;
    pin_ = pin;
    ready_.store(process.IsValid());
    return process;
  }

  void Read() {
    std::array<char, 8192> chunk;
    std::string pending;
    while (!stopped_.load()) {
      DWORD bytes = 0;
      if (!ReadFile(input_.get(), chunk.data(), chunk.size(), &bytes,
                    nullptr) ||
          bytes == 0) {
        return;
      }
      pending.append(chunk.data(), bytes);
      size_t start = 0;
      for (size_t end = pending.find('\n'); end != std::string::npos;
           end = pending.find('\n', start)) {
        if (end - start > kMaxFrame) {
          return;
        }
        auto value = base::JSONReader::ReadDict(
            std::string_view(pending).substr(start, end - start),
            base::JSON_PARSE_RFC);
        if (!value || value->FindInt("v") != 1 ||
            replies_.fetch_add(1) >= kMaxPending ||
            !main_->PostTask(
                FROM_HERE,
                base::BindOnce(&State::Deliver, base::WrapRefCounted(this),
                               std::move(*value)))) {
          return;
        }
        start = end + 1;
      }
      pending.erase(0, start);
      if (pending.size() > kMaxFrame) {
        return;
      }
    }
  }

  void Deliver(base::DictValue value) {
    replies_.fetch_sub(1);
    if (!stopped_.load()) {
      message_.Run(std::move(value));
    }
  }

  void Write(std::string data) {
    pending_.fetch_sub(1);
    if (!ready_.load() || stopped_.load()) {
      return;
    }
    DWORD written = 0;
    if (!WriteFile(output_.get(), data.data(), static_cast<DWORD>(data.size()),
                   &written, nullptr) ||
        written != data.size()) {
      Stop();
    }
  }

  const scoped_refptr<base::SequencedTaskRunner> main_;
  const scoped_refptr<base::SequencedTaskRunner> writer_;
  const Message message_;
  Closed closed_;
  Ready on_ready_;
  std::atomic_bool stopped_{false};
  std::atomic_bool ready_{false};
  std::atomic_int pending_{0};
  std::atomic_int replies_{0};
  base::Lock mutex_;
  base::win::ScopedHandle job_ GUARDED_BY(mutex_);
  base::Process supervisor_ GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<LaunchAuthorization>> panel_authorizations_
      GUARDED_BY(mutex_);
  base::FilePath directory_;
  std::optional<ComponentPin> pin_;
  base::win::ScopedHandle input_;
  base::win::ScopedHandle output_;
};

SidecarProcess::SidecarProcess(scoped_refptr<State> state)
    : state_(std::move(state)) {}

SidecarProcess::~SidecarProcess() {
  state_->Stop();
}

std::unique_ptr<SidecarProcess> SidecarProcess::Start(
    const base::FilePath& path,
    const ComponentPin& pin,
    std::optional<ProviderSandbox> sandbox,
    Message message,
    Closed closed,
    Ready ready) {
  auto state = base::MakeRefCounted<State>(std::move(message),
                                           std::move(closed), std::move(ready));
  if (!base::ThreadPool::PostTask(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&State::Run, state, path, pin, std::move(sandbox)))) {
    return nullptr;
  }
  return std::unique_ptr<SidecarProcess>(new SidecarProcess(std::move(state)));
}

bool SidecarProcess::Send(base::DictValue message) {
  return state_->Send(std::move(message));
}

bool SidecarProcess::AuthorizeProviders(
    std::string profile_token,
    std::vector<std::string> instances,
    std::vector<ProviderLaunchRequest> requests,
    Ready done) {
  return state_->AuthorizeProviders(std::move(profile_token),
                                    std::move(instances), std::move(requests),
                                    std::move(done));
}

}  // namespace krypton_agent
