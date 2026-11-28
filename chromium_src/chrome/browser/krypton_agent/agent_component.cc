#include "chrome/browser/krypton_agent/agent_component.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/base_paths_win.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "base/win/trust_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"
#include "chrome/browser/krypton_models/files.h"
#include "components/krypton_privacy_features/privacy_features.h"
#include "components/prefs/pref_service.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace krypton_agent {
namespace {

constexpr uint64_t kMaximumSize = 256 * 1024 * 1024;
constexpr uint64_t kMaximumLauncherSize = 32 * 1024 * 1024;
constexpr char kUnsignedDevelopmentComponent[] =
    "krypton-agent-unsigned-dev-component";
constexpr base::FilePath::CharType kDevelopmentDirectory[] =
    FILE_PATH_LITERAL("krypton-agent-dev-component");
constexpr net::NetworkTrafficAnnotationTag kTraffic =
    net::DefineNetworkTrafficAnnotation("krypton_agent_component",
                                        R"(
      semantics {
        sender: "Krypton Agent Component"
        description: "Downloads the version of the signed agent component pinned in this build."
        trigger: "The user opts in to Krypton Agent and requests installation."
        data: "No cookies, page content, credentials, or model prompts."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting: "AI agent in Krypton's privacy settings. It is off by default."
        policy_exception_justification: "The feature requires explicit opt-in."
      })");

base::FilePath AgentRoot() {
  base::FilePath local;
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local)) {
    return {};
  }
  return local.AppendASCII("Krypton").AppendASCII("Agent");
}

base::FilePath DevelopmentComponentDirectory() {
  base::FilePath executable_directory;
  if (!base::PathService::Get(base::DIR_EXE, &executable_directory)) {
    return {};
  }
  return executable_directory.Append(kDevelopmentDirectory);
}

bool IsSha256(std::string_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
           return base::IsHexDigit(ch);
         });
}

bool ValidPin(const ComponentPin& pin) {
  const GURL url(pin.url);
  return !pin.version.empty() && pin.version.size() <= 50 &&
         std::ranges::all_of(
             pin.version,
             [](char ch) { return base::IsAsciiDigit(ch) || ch == '.'; }) &&
         base::IsAsciiDigit(pin.version.front()) &&
         base::IsAsciiDigit(pin.version.back()) &&
         (pin.unsigned_development
              ? pin.url.empty()
              : url.SchemeIs("https") && !url.has_username() &&
                    !url.has_password()) &&
         pin.size > 0 && pin.size <= kMaximumSize && IsSha256(pin.sha256) &&
         pin.protocol == 1 && pin.launcher_size > 0 &&
         pin.launcher_size <= kMaximumLauncherSize &&
         IsSha256(pin.launcher_sha256);
}

// The pin borrows the process command line value stored by the caller.
std::optional<ComponentPin> ParseDevelopmentPin(std::string_view value) {
  if (value.size() > 224) {
    return std::nullopt;
  }
  const auto fields = base::SplitStringPiece(value, ",", base::KEEP_WHITESPACE,
                                             base::SPLIT_WANT_ALL);
  if (fields.size() != 5) {
    return std::nullopt;
  }
  ComponentPin pin{.version = fields[0],
                   .url = {},
                   .size = 0,
                   .sha256 = fields[2],
                   .protocol = 1,
                   .launcher_size = 0,
                   .launcher_sha256 = fields[4],
                   .unsigned_development = true};
  if (!base::StringToUint64(fields[1], &pin.size) ||
      !base::StringToUint64(fields[3], &pin.launcher_size) || !ValidPin(pin)) {
    return std::nullopt;
  }
  return pin;
}

bool VerifyComponentFile(const base::FilePath& image,
                         const ComponentPin& pin,
                         uint64_t size,
                         std::string_view hash,
                         uint64_t maximum) {
  if (!ValidPin(pin) ||
      !krypton::models::VerifyFile(image, size, hash, maximum)) {
    return false;
  }
  if (pin.unsigned_development) {
    const auto directory = DevelopmentComponentDirectory();
    return !directory.empty() &&
           base::FilePath::CompareEqualIgnoreCase(directory.value(),
                                                  image.DirName().value());
  }
  return base::win::IsBinaryTrusted(image, /*verify_publisher=*/true,
                                    /*force_verify_in_dev_builds=*/true);
}

}  // namespace

struct AgentComponent::Paths {
  base::FilePath image;
  base::FilePath partial;
  bool verified = false;
  bool launcher_verified = false;
};

const ComponentPin* PinnedAgentComponent() {
  const auto& command = *base::CommandLine::ForCurrentProcess();
  if (command.HasSwitch(kUnsignedDevelopmentComponent)) {
    static const base::NoDestructor<std::string> argument(
        command.GetSwitchValueASCII(kUnsignedDevelopmentComponent));
    static const auto pin = ParseDevelopmentPin(*argument);
    return pin ? &*pin : nullptr;
  }
  // Add the signed release's pin here when a sidecar release is available.
  // No URL or executable is accepted from preferences or IPC.
  return nullptr;
}

bool IsProductionAgentEnabled() {
  using krypton::privacy::Id;
  const auto* prefs =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  return prefs && krypton::privacy::IsStartupEnabled(Id::kAiAgent) &&
         krypton::privacy::IsEnabled(*prefs, Id::kAiAgent);
}

bool VerifyAgentImage(const base::FilePath& image, const ComponentPin& pin) {
  return VerifyComponentFile(image, pin, pin.size, pin.sha256, kMaximumSize);
}

bool VerifyAgentLauncher(const base::FilePath& directory,
                         const ComponentPin& pin) {
  const auto image = directory.AppendASCII("krypton-agent-launcher.exe");
  return VerifyComponentFile(image, pin, pin.launcher_size, pin.launcher_sha256,
                             kMaximumLauncherSize);
}

AgentComponent::AgentComponent() = default;
AgentComponent::~AgentComponent() {
  Cancel();
}

void AgentComponent::Ensure(
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    Callback callback,
    Progress progress) {
  if (!IsProductionAgentEnabled()) {
    std::move(callback).Run(base::unexpected(Error::kDisabled));
    return;
  }
  const ComponentPin* pin = PinnedAgentComponent();
  if (!pin || !ValidPin(*pin)) {
    std::move(callback).Run(base::unexpected(Error::kNoPinnedRelease));
    return;
  }
  if (callback_) {
    std::move(callback).Run(base::unexpected(Error::kBusy));
    return;
  }
  callback_ = std::move(callback);
  progress_ = std::move(progress);
  if (progress_) {
    progress_.Run(true, 0);
  }
  const bool allow_download = factory && !pin->unsigned_development;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](const ComponentPin* pin,
             bool allow_download) -> std::unique_ptr<Paths> {
            base::FilePath local;
            if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local) ||
                !PrepareAgentPrivateDirectories(local)) {
              return nullptr;
            }
            base::FilePath directory;
            if (pin->unsigned_development) {
              directory = DevelopmentComponentDirectory();
            } else if (auto root = AgentRoot(); !root.empty()) {
              directory = root.AppendASCII("bin").AppendASCII(pin->version);
            }
            if (directory.empty()) {
              return nullptr;
            }
            auto paths = std::make_unique<Paths>();
            paths->image = directory.AppendASCII("krypton-agent.exe");
            paths->launcher_verified = VerifyAgentLauncher(directory, *pin);
            if (!paths->launcher_verified) {
              return paths;
            }
            paths->verified = VerifyAgentImage(paths->image, *pin);
            if (paths->verified || !allow_download) {
              return paths;
            }
            if (!base::CreateDirectory(directory) ||
                !base::DeleteFile(paths->image)) {
              return nullptr;
            }
            paths->partial = directory.AppendASCII(
                base::UnguessableToken::Create().ToString() + ".partial");
            return paths;
          },
          pin, allow_download),
      base::BindOnce(&AgentComponent::OnPrepared, weak_factory_.GetWeakPtr(),
                     std::move(factory), pin));
}

void AgentComponent::OnPrepared(
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    const ComponentPin* pin,
    std::unique_ptr<Paths> paths) {
  if (!IsProductionAgentEnabled()) {
    Finish(base::unexpected(Error::kDisabled));
    return;
  }
  if (!paths) {
    Finish(base::unexpected(Error::kDirectory));
    return;
  }
  if (!paths->launcher_verified) {
    Finish(base::unexpected(Error::kVerification));
    return;
  }
  if (paths->verified) {
    Finish(paths->image);
    return;
  }
  if (pin->unsigned_development || !factory) {
    Finish(base::unexpected(pin->unsigned_development ? Error::kVerification
                                                      : Error::kDownload));
    return;
  }
  paths_ = std::move(paths);
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(pin->url);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->redirect_mode = network::mojom::RedirectMode::kError;
  request->load_flags = net::LOAD_DISABLE_CACHE;
  download_ = network::SimpleURLLoader::Create(std::move(request), kTraffic);
  download_->SetTimeoutDuration(base::Minutes(10));
  if (progress_) {
    progress_.Run(false, 0);
    download_->SetOnDownloadProgressCallback(base::BindRepeating(
        [](Progress progress, uint64_t total, uint64_t downloaded) {
          progress.Run(false,
                       std::min(1.0, static_cast<double>(downloaded) / total));
        },
        progress_, pin->size));
  }
  download_->DownloadToFile(factory.get(),
                            base::BindOnce(&AgentComponent::OnDownloaded,
                                           weak_factory_.GetWeakPtr(), pin),
                            paths_->partial, static_cast<int64_t>(pin->size));
}

void AgentComponent::OnDownloaded(const ComponentPin* pin,
                                  base::FilePath file) {
  download_.reset();
  if (progress_) {
    progress_.Run(true, 1);
  }
  if (file.empty()) {
    Finish(base::unexpected(Error::kDownload));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](const ComponentPin* pin, const base::FilePath& partial) {
            return VerifyAgentImage(partial, *pin);
          },
          pin, file),
      base::BindOnce(
          [](base::WeakPtr<AgentComponent> owner, base::FilePath file,
             bool valid) {
            if (!owner) {
              base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                                         base::GetDeleteFileCallback(file));
              return;
            }
            if (!valid || !IsProductionAgentEnabled()) {
              owner->Finish(base::unexpected(valid ? Error::kDisabled
                                                   : Error::kVerification));
              return;
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE, {base::MayBlock()},
                base::BindOnce(
                    [](const base::FilePath& partial,
                       const base::FilePath& image)
                        -> base::expected<base::FilePath, Error> {
                      if (base::ReplaceFile(partial, image, nullptr)) {
                        return image;
                      }
                      return base::unexpected(Error::kDirectory);
                    },
                    file, owner->paths_->image),
                base::BindOnce(&AgentComponent::Finish, owner));
          },
          weak_factory_.GetWeakPtr(), file));
}

void AgentComponent::Cancel() {
  progress_.Reset();
  weak_factory_.InvalidateWeakPtrs();
  download_.reset();
  if (paths_ && !paths_->partial.empty()) {
    base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                               base::GetDeleteFileCallback(paths_->partial));
  }
  paths_.reset();
  callback_.Reset();
}

void AgentComponent::Finish(base::expected<base::FilePath, Error> result) {
  auto callback = std::move(callback_);
  Cancel();
  std::move(callback).Run(std::move(result));
}

void RemoveAgentComponent() {
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce([] {
        const auto root = AgentRoot();
        base::FilePath local;
        if (!root.empty() &&
            base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local) &&
            RemoveAllProviderSandboxes(local)) {
          base::DeletePathRecursively(root);
        }
      }));
}

}  // namespace krypton_agent
