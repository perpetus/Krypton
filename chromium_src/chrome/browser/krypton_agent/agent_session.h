#ifndef CHROME_BROWSER_KRYPTON_AGENT_AGENT_SESSION_H_
#define CHROME_BROWSER_KRYPTON_AGENT_AGENT_SESSION_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/weak_document_ptr.h"
#include "url/gurl.h"

class Profile;
class BrowserWindowInterface;

namespace content {
class WebContents;
}

namespace gfx {
class Rect;
}

namespace tabs {
class TabInterface;
}

namespace krypton_agent {

class ChatCard;

inline constexpr char kAgentNewTabUrl[] = "about:blank#krypton-agent";

using ReviewHandoff =
    base::RepeatingCallback<void(std::string reason,
                                 base::OnceCallback<void(bool)> decision)>;

using ReviewAction =
    base::RepeatingCallback<void(base::DictValue action,
                                 base::OnceCallback<void(bool)> decision)>;
enum class SignInDecision { kDeny, kAllowOnce, kAllowInChat };
using ReviewSignIn = base::RepeatingCallback<
    void(std::string site, base::OnceCallback<void(SignInDecision)> decision)>;

// A native user share of one document in the session window for one turn.
struct ContextTab {
  static std::optional<ContextTab> Capture(tabs::TabInterface& tab,
                                           BrowserWindowInterface& window);
  content::WebContents* GetContents(BrowserWindowInterface& window) const;

  tabs::TabHandle tab;
  content::WeakDocumentPtr document;
  GURL url;
};

struct SessionOptions {
  std::string provider;
  std::string model;
  // The model's T3 reasoning option and the chosen level; empty for the
  // model's default.
  std::string reasoning_option;
  std::string reasoning;
  std::string prompt;
  std::string resume_session_id;
  std::optional<ContextTab> context_tab;
  bool allow_actions = false;
  bool allow_research = false;
  bool allow_tab_list = false;
  ReviewAction review;
  ReviewHandoff handoff;
  ReviewSignIn review_sign_in;
  base::RepeatingCallback<void(std::string)> suggest_prompt;
  // Populated by native browser code for the selected provider instance.
  ProviderProgram approved_program;
  ProviderIsolation isolation = ProviderIsolation::kAppContainer;
  bool unsandboxed_development_provider = false;
  bool restricted_provider = true;
  bool provider_web_access = true;
};

using SessionEvent = base::RepeatingCallback<void(base::DictValue)>;

// Browser-side entry points for an explicit user session. The caller owns
// presentation and provider selection.
bool StartAgentSession(tabs::TabInterface& tab,
                       SessionOptions options,
                       SessionEvent events);
// `model`, when not empty, switches the session's model within its provider
// from this turn on.
bool SendAgentMessage(std::string message,
                      std::string model = {},
                      std::string reasoning_option = {},
                      std::string reasoning = {},
                      std::optional<ContextTab> context_tab = std::nullopt);
bool SetAgentProviderConsent(Profile& profile,
                             std::string_view provider,
                             bool provider_web_access,
                             bool allowed,
                             bool job_only = false);
using AgentHistoryList =
    base::OnceCallback<void(std::optional<base::ListValue>)>;
bool ListAgentSavedSessions(Profile& profile, AgentHistoryList reply);
using SavedTranscriptReply =
    base::OnceCallback<void(std::optional<base::DictValue>)>;
bool OpenAgentSavedTranscript(Profile& profile,
                              std::string session_id,
                              int cursor,
                              SavedTranscriptReply reply);
bool SetAgentSessionArchived(Profile& profile,
                             std::string session_id,
                             bool archived,
                             base::OnceCallback<void(bool)> reply);
// An empty selector pair deletes all history for this profile.
bool DeleteAgentSavedSessions(Profile& profile,
                              std::optional<std::string> session_id,
                              std::optional<std::string> space_id,
                              base::OnceCallback<void(bool)> reply);
content::WebContents* GetAgentChatContents();
bool HasAgentChat(BrowserWindowInterface& window);
bool HasRunningAgentSession(BrowserWindowInterface& window);
bool AgentChatUsesTab(tabs::TabInterface& tab);
bool IsAgentWorkingOnTab(tabs::TabInterface& tab);
bool PlaceAgentChatCard(ChatCard& card,
                        BrowserWindowInterface& window,
                        const gfx::Rect& bounds);
bool CanStartAgentSession(tabs::TabInterface& tab);
bool StopAgentTurn(BrowserWindowInterface& window);
void CloseAgentChat(BrowserWindowInterface& window);
// Reserved browser-client bounds supplied by native layout. An empty rect
// detaches the transcript. The host never picks an overlapping position.
bool SetAgentChatBounds(BrowserWindowInterface& window,
                        const gfx::Rect& bounds);
bool OpenAgentChatForWindow(BrowserWindowInterface& window);
void SetAgentChatTheme(bool dark);
void StopAgentSession();

void MaybeStartDevelopmentSession();
void StopDevelopmentSession();

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_AGENT_SESSION_H_
