#ifndef CHROME_BROWSER_KRYPTON_AGENT_RESEARCH_LOAD_H_
#define CHROME_BROWSER_KRYPTON_AGENT_RESEARCH_LOAD_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/actor/actor_surface.h"
#include "chrome/browser/actor/tab_observation_strategy.h"
#include "chrome/browser/krypton_agent/confirmation_gate.h"
#include "chrome/browser/krypton_agent/ui_state_manager.h"
#include "chrome/common/actor/action_result.h"
#include "components/actor/core/shared_types.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

class Profile;

namespace actor {
class ActorKeyedService;
}

namespace krypton_agent {

// Owns the task and offscreen surface for one research document.
class ResearchLoad final : public content::WebContentsDelegate,
                           public content::WebContentsObserver {
 public:
  enum class Result { kLoaded, kSignIn, kBlocked, kFailed, kTimeout };
  using RedirectCheck = base::RepeatingCallback<bool(const GURL&)>;
  using Completion = base::OnceCallback<void(Result)>;

  ResearchLoad(Profile& profile,
               actor::ActorKeyedService& service,
               GURL destination,
               const std::string& partition_domain,
               bool use_sign_in,
               RedirectCheck check_redirect,
               Completion completion);
  ~ResearchLoad() override;

  void Start();

 private:
  bool CheckNavigation(content::NavigationHandle& navigation, bool redirected);
  void OnBound(actor::mojom::ActionResultPtr result);
  void OnNavigated(std::vector<actor::ActionResultWithLatencyInfo> results,
                   actor::TabObservationStrategy observation);
  void MaybeFinishLoading();
  void Complete(Result result);

  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void DidStartLoading() override;
  void DidStopLoading() override;
  void WebContentsDestroyed() override;

  bool ShouldSuppressDialogs(content::WebContents*) override;
  bool IsWebContentsCreationOverridden(content::RenderFrameHost*,
                                       content::SiteInstance*,
                                       content::mojom::WindowContainerType,
                                       const GURL&,
                                       const std::string&,
                                       const GURL&) override;
  void CanDownload(const GURL&,
                   const std::string&,
                   base::OnceCallback<void(bool)> callback) override;
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)> callback) override;

  raw_ptr<actor::ActorKeyedService> service_;
  const GURL destination_;
  const bool use_sign_in_;
  RedirectCheck check_redirect_;
  Completion completion_;
  actor::ActorSurfaceHandle surface_;
  actor::TaskId task_id_;
  std::optional<int64_t> navigation_id_;
  std::optional<Result> navigation_failure_;
  bool navigated_ = false;
  ConfirmationGate gate_;
  UiStateManager ui_;
  base::OneShotTimer timeout_;
  base::OneShotTimer quiet_;
  base::WeakPtrFactory<ResearchLoad> weak_factory_{this};
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_RESEARCH_LOAD_H_
