#include "chrome/browser/krypton_agent/browser_prompt_gate.h"

#include <memory>
#include <utility>

#include "base/supports_user_data.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/web_contents.h"

namespace krypton_agent {
namespace {

constexpr int kPromptGate = 0;
constexpr int kHiddenResearch = 0;

class HiddenResearchThrottle final : public content::NavigationThrottle {
 public:
  explicit HiddenResearchThrottle(content::NavigationThrottleRegistry& registry)
      : NavigationThrottle(registry) {}

  ThrottleCheckResult WillStartRequest() override { return Check(false); }
  ThrottleCheckResult WillRedirectRequest() override { return Check(true); }
  ThrottleCheckResult WillCommitWithoutUrlLoader() override {
    return Check(false);
  }
  const char* GetNameForLogging() override { return "KryptonHiddenResearch"; }

 private:
  ThrottleCheckResult Check(bool redirected) {
    return AllowAgentNavigation(*navigation_handle(), redirected)
               ? PROCEED
               : CANCEL_AND_IGNORE;
  }
};

class PromptGate final : public base::SupportsUserData::Data {
 public:
  PromptGate(
      base::RepeatingCallback<bool()> pause,
      base::RepeatingCallback<bool(content::NavigationHandle&, bool)> navigate)
      : pause(std::move(pause)), navigate(std::move(navigate)) {}

  const base::RepeatingCallback<bool()> pause;
  const base::RepeatingCallback<bool(content::NavigationHandle&, bool)>
      navigate;
};

}  // namespace

bool PauseForBrowserPrompt(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  auto* gate = static_cast<PromptGate*>(contents->GetUserData(&kPromptGate));
  const auto callback = gate ? gate->pause : base::RepeatingCallback<bool()>();
  return callback && callback.Run();
}

bool BindBrowserPromptGate(
    content::WebContents& contents,
    base::RepeatingCallback<bool()> pause,
    base::RepeatingCallback<bool(content::NavigationHandle&, bool)> navigate) {
  if (contents.GetUserData(&kPromptGate)) {
    return false;
  }
  contents.SetUserData(
      &kPromptGate,
      std::make_unique<PromptGate>(std::move(pause), std::move(navigate)));
  return true;
}

bool AllowAgentNavigation(content::NavigationHandle& navigation,
                          bool redirected) {
  auto* gate = static_cast<PromptGate*>(
      navigation.GetWebContents()->GetUserData(&kPromptGate));
  return gate && gate->navigate.Run(navigation, redirected);
}

void UnbindBrowserPromptGate(content::WebContents& contents) {
  contents.RemoveUserData(&kPromptGate);
}

void MarkHiddenResearch(content::WebContents& contents) {
  contents.SetUserData(&kHiddenResearch,
                       std::make_unique<base::SupportsUserData::Data>());
}

bool IsHiddenResearch(const content::WebContents* contents) {
  return contents && contents->GetUserData(&kHiddenResearch);
}

void AddHiddenResearchThrottle(content::NavigationThrottleRegistry& registry) {
  if (IsHiddenResearch(registry.GetNavigationHandle().GetWebContents())) {
    registry.AddThrottle(std::make_unique<HiddenResearchThrottle>(registry));
  }
}

}  // namespace krypton_agent
