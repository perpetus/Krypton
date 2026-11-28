#ifndef CHROME_BROWSER_KRYPTON_AGENT_BROWSER_PROMPT_GATE_H_
#define CHROME_BROWSER_KRYPTON_AGENT_BROWSER_PROMPT_GATE_H_

#include "base/functional/callback.h"

namespace content {
class NavigationHandle;
class NavigationThrottleRegistry;
class WebContents;
}  // namespace content

namespace krypton_agent {

// Returns true when a request was blocked because the task controlled the tab.
// The native caller cancels the pending request and lets the user retry it.
bool PauseForBrowserPrompt(content::WebContents* contents);
bool AllowAgentNavigation(content::NavigationHandle& navigation,
                          bool redirected);
bool BindBrowserPromptGate(
    content::WebContents& contents,
    base::RepeatingCallback<bool()> pause,
    base::RepeatingCallback<bool(content::NavigationHandle&, bool)> navigate);
void UnbindBrowserPromptGate(content::WebContents& contents);
void MarkHiddenResearch(content::WebContents& contents);
bool IsHiddenResearch(const content::WebContents* contents);
void AddHiddenResearchThrottle(content::NavigationThrottleRegistry& registry);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_BROWSER_PROMPT_GATE_H_
