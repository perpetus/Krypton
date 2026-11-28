#include "chrome/browser/krypton_agent/research_load.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/task/bind_post_task.h"
#include "base/time/time.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_surface_registry.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "chrome/browser/actor/enterprise_policy_checker.h"
#include "chrome/browser/actor/tools/navigate_tool_request.h"
#include "chrome/browser/krypton_agent/browser_prompt_gate.h"
#include "chrome/browser/profiles/profile.h"
#include "components/actor/core/task_source_info.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "net/http/http_response_headers.h"
#include "pdf/buildflags.h"
#include "ui/gfx/geometry/rect.h"
#include "url/origin.h"

#if BUILDFLAG(ENABLE_PDF)
#include "extensions/common/constants.h"
#endif

namespace krypton_agent {
namespace {

bool IsSignInAddress(const GURL& address) {
  const std::string path = base::ToLowerASCII(address.path());
  for (const auto* prefix : {"/login", "/signin", "/sign-in", "/oauth"}) {
    if (path == prefix ||
        (path.starts_with(prefix) &&
         path[std::char_traits<char>::length(prefix)] == '/')) {
      return true;
    }
  }
  return false;
}

bool IsPdfViewerNavigation(content::NavigationHandle& navigation,
                           bool redirected) {
#if BUILDFLAG(ENABLE_PDF)
  auto* contents = navigation.GetWebContents();
  auto* parent = navigation.GetParentFrame();
  if (redirected || !parent || navigation.IsPost() ||
      contents->GetContentsMimeType() != "application/pdf") {
    return false;
  }
  const auto& target = navigation.GetURL();
  const auto viewer =
      url::Origin::Create(GURL(std::string(extensions::kExtensionScheme) +
                               "://" + extension_misc::kPdfExtensionId));
  if (parent == contents->GetPrimaryMainFrame()) {
    return target.IsAboutBlank() || (!navigation.IsRendererInitiated() &&
                                     url::Origin::Create(target) == viewer &&
                                     target.path() == "/index.html");
  }
  return parent->GetParent() == contents->GetPrimaryMainFrame() &&
         parent->GetLastCommittedOrigin() == viewer &&
         ((target.SchemeIsBlob() && url::Origin::Create(target) == viewer) ||
          (navigation.IsPdf() && !navigation.IsRendererInitiated() &&
           target == contents->GetLastCommittedURL()));
#else
  return false;
#endif
}

}  // namespace

ResearchLoad::ResearchLoad(Profile& profile,
                           actor::ActorKeyedService& service,
                           GURL destination,
                           const std::string& partition_domain,
                           bool use_sign_in,
                           RedirectCheck check_redirect,
                           Completion completion)
    : service_(&service),
      destination_(std::move(destination)),
      use_sign_in_(use_sign_in),
      check_redirect_(std::move(check_redirect)),
      completion_(base::BindPostTaskToCurrentDefault(std::move(completion))) {
  auto site = use_sign_in_
                  ? content::SiteInstance::CreateForURL(&profile, destination_)
                  : content::SiteInstance::CreateForFixedStoragePartition(
                        &profile, destination_,
                        content::StoragePartitionConfig::Create(
                            &profile, partition_domain, "research", true));
  auto* surface =
      service_->GetSurfaceRegistry().CreateHiddenWebContents(std::move(site));
  surface_ = surface->GetHandle();
  auto* contents = surface->GetWebContents();
  Observe(contents);
  contents->Resize(gfx::Rect(1280, 900));
  MarkHiddenResearch(*contents);
  contents->SetDelegate(this);
}

ResearchLoad::~ResearchLoad() {
  weak_factory_.InvalidateWeakPtrs();
  if (!task_id_.is_null() && service_->GetTask(task_id_)) {
    service_->StopTask(task_id_,
                       actor::ActorTask::StoppedReason::kTaskComplete);
  }
  if (web_contents()) {
    web_contents()->SetDelegate(nullptr);
    Observe(nullptr);
    service_->GetSurfaceRegistry().DestroySurface(surface_);
  }
}

void ResearchLoad::Start() {
  auto* contents = web_contents();
  if (!contents ||
      !BindBrowserPromptGate(
          *contents, base::BindRepeating([] { return true; }),
          base::BindRepeating(
              [](base::WeakPtr<ResearchLoad> load,
                 content::NavigationHandle& navigation, bool redirected) {
                return load && load->CheckNavigation(navigation, redirected);
              },
              weak_factory_.GetWeakPtr()))) {
    Complete(Result::kBlocked);
    return;
  }
  task_id_ = service_->CreateTaskWithOptions(
      actor::TaskSourceInfo(actor::TaskSourceInfo::Client::kKrypton,
                            "krypton-research"),
      actor::GetNullEnterprisePolicyChecker(), nullptr, gate_.GetWeakPtr(),
      &ui_, std::nullopt,
      actor::ActorKeyedService::AllowedSchemes::kRequireHttpsOrHttp);
  auto* task = service_->GetTask(task_id_);
  if (!task) {
    Complete(Result::kFailed);
    return;
  }
  timeout_.Start(FROM_HERE, base::Seconds(30),
                 base::BindOnce(&ResearchLoad::Complete,
                                weak_factory_.GetWeakPtr(), Result::kTimeout));
  task->AddActorSurface(
      surface_, /*stop_task_on_detach=*/true,
      base::BindOnce(&ResearchLoad::OnBound, weak_factory_.GetWeakPtr()));
}

void ResearchLoad::OnBound(actor::mojom::ActionResultPtr result) {
  if (!completion_) {
    return;
  }
  if (!result || !actor::IsOk(result->code) || !service_->GetTask(task_id_)) {
    Complete(Result::kFailed);
    return;
  }
  std::vector<std::unique_ptr<actor::ToolRequest>> actions;
  actions.push_back(
      std::make_unique<actor::NavigateToolRequest>(surface_, destination_));
  service_->PerformActions(
      task_id_, std::move(actions), actor::ActorTaskMetadata(),
      base::BindOnce(&ResearchLoad::OnNavigated, weak_factory_.GetWeakPtr()));
}

bool ResearchLoad::CheckNavigation(content::NavigationHandle& navigation,
                                   bool redirected) {
  const auto& destination = navigation.GetURL();
  if (!navigation.IsInPrimaryMainFrame()) {
    return IsPdfViewerNavigation(navigation, redirected);
  }
  if (!completion_ || navigation.IsRendererInitiated() || navigation.IsPost() ||
      navigation.IsSameDocument() ||
      !ConfirmationGate::IsWebNavigation(destination)) {
    return false;
  }
  if (!navigation_id_) {
    if (redirected || destination != destination_) {
      return false;
    }
    navigation_id_ = navigation.GetNavigationId();
  }
  if (*navigation_id_ != navigation.GetNavigationId()) {
    return false;
  }
  if (redirected && use_sign_in_ && !destination.SchemeIs("https")) {
    return false;
  }
  if (redirected &&
      (url::Origin::Create(destination) != url::Origin::Create(destination_) ||
       !check_redirect_.Run(destination))) {
    navigation_failure_ = (!use_sign_in_ && IsSignInAddress(destination))
                              ? Result::kSignIn
                              : Result::kBlocked;
    return false;
  }
  return true;
}

void ResearchLoad::DidFinishNavigation(content::NavigationHandle* navigation) {
  if (!navigation_id_ || navigation->GetNavigationId() != *navigation_id_) {
    return;
  }
  const auto* headers = navigation->GetResponseHeaders();
  if (headers &&
      (headers->response_code() == 401 || headers->response_code() == 403)) {
    navigation_failure_ = Result::kSignIn;
  }
}

void ResearchLoad::OnNavigated(
    std::vector<actor::ActionResultWithLatencyInfo> results,
    actor::TabObservationStrategy /*observation*/) {
  if (!completion_) {
    return;
  }
  if (navigation_failure_) {
    Complete(*navigation_failure_);
    return;
  }
  if (results.empty() || std::ranges::any_of(results, [](const auto& result) {
        return !result.result || !actor::IsOk(result.result->code);
      })) {
    Complete(Result::kFailed);
    return;
  }
  navigated_ = true;
  MaybeFinishLoading();
}

void ResearchLoad::DidStartLoading() {
  quiet_.Stop();
}

void ResearchLoad::DidStopLoading() {
  MaybeFinishLoading();
}

void ResearchLoad::MaybeFinishLoading() {
  if (completion_ && navigated_ && web_contents() &&
      !web_contents()->IsLoading()) {
    quiet_.Start(FROM_HERE, base::Milliseconds(350),
                 base::BindOnce(&ResearchLoad::Complete,
                                weak_factory_.GetWeakPtr(), Result::kLoaded));
  }
}

void ResearchLoad::Complete(Result result) {
  quiet_.Stop();
  timeout_.Stop();
  if (completion_) {
    std::move(completion_).Run(result);
  }
}

void ResearchLoad::WebContentsDestroyed() {
  Observe(nullptr);
  Complete(Result::kFailed);
}

bool ResearchLoad::ShouldSuppressDialogs(content::WebContents*) {
  return true;
}

bool ResearchLoad::IsWebContentsCreationOverridden(
    content::RenderFrameHost*,
    content::SiteInstance*,
    content::mojom::WindowContainerType,
    const GURL&,
    const std::string&,
    const GURL&) {
  return true;
}

void ResearchLoad::CanDownload(const GURL&,
                               const std::string&,
                               base::OnceCallback<void(bool)> callback) {
  std::move(callback).Run(false);
}

content::WebContents* ResearchLoad::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)> callback) {
  if (!completion_ || navigation_id_ || source != web_contents() ||
      params.is_renderer_initiated || params.url != destination_) {
    return nullptr;
  }
  content::NavigationController::LoadURLParams load(params);
  load.has_user_gesture = false;
  auto navigation = source->GetController().LoadURLWithParams(load);
  if (!navigation) {
    return nullptr;
  }
  if (callback) {
    std::move(callback).Run(*navigation);
  }
  return source;
}

}  // namespace krypton_agent
