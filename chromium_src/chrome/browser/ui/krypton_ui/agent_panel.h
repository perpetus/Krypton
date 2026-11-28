#ifndef CHROME_BROWSER_UI_KRYPTON_UI_AGENT_PANEL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_AGENT_PANEL_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/krypton_agent/agent_session.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/prefs/pref_change_registrar.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/gfx/geometry/rect.h"

namespace base {
class FilePath;
}

class BrowserWindowInterface;
class Profile;

namespace krypton_agent {
class ChatCard;
struct TranscriptEntry;
class PanelConnection;
class SkillImportPicker;
}  // namespace krypton_agent

namespace krypton_ui {
class WindowSession;

class AgentPanel final : public content::WebContentsObserver {
 public:
  AgentPanel(BrowserWindowInterface& window, WindowSession& session);
  ~AgentPanel() override;

  void SetVisible(bool visible);
  void SetBounds(KrRect bounds);
  void SetDark(bool dark);
  void UpdateActiveTab();
  void PrivacyChanged();
  bool Show();
  KrResult Command(const KrAgentCommand* command);
  KrResult Respond(KrAgentRecordId id, uint32_t decision);
  uint32_t SiteFlags(KrStr entry) const;

 private:
  using RecordMap = std::map<KrAgentRecordId, base::DictValue>;
  using Names = std::map<std::string, KrAgentRecordId, std::less<>>;
  using Decision = base::OnceCallback<void(bool)>;
  struct Review {
    uint32_t kind;
    uint32_t flags;
    std::string title;
    std::string detail;
    std::string target;
    std::string always_allow;
    Decision decision;
    base::OnceCallback<void(krypton_agent::SignInDecision)> sign_in_decision =
        {};
  };

  Profile& profile() const;
  const KrShellUI* api() const;
  void SuggestPrompt(unsigned generation, std::string text);
  bool Enabled() const;
  void PublishState();
  void PrefChanged();
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void WebContentsDestroyed() override;
  bool Start(const KrAgentCommand& command, std::string_view resume);
  void ResetShownSession();
  void OnEvent(unsigned generation, base::DictValue event);
  void OnControlEvent(base::DictValue event);
  void ReviewAction(unsigned generation,
                    base::DictValue action,
                    Decision decision);
  void ReviewHandoff(unsigned generation,
                     std::string reason,
                     Decision decision);
  void ReviewSignIn(
      unsigned generation,
      std::string site,
      base::OnceCallback<void(krypton_agent::SignInDecision)> decision);
  void AddReview(Review review);
  void PublishReview(KrAgentRecordId id, const Review& review);
  void ClearReviews();
  void PublishActivity(KrAgentRecordId id, const base::DictValue& activity);
  void RefreshHistory();
  void OnHistory(unsigned generation, std::optional<base::ListValue> sessions);
  void OnHistoryChanged(bool success);
  void PublishSites();
  bool RefreshProviders(bool force);
  void OnProviderInventory(std::optional<base::DictValue> result);
  void OnProviders(std::optional<base::DictValue> result);
  void PublishProviders();
  void RefreshSkills();
  void OnSkills(std::optional<base::DictValue> result);
  bool MutateSkill(std::string method, base::DictValue params);
  void SkillSelected(base::FilePath path);
  void OnMutation(std::optional<base::DictValue> result);
  bool OpenSaved(KrAgentRecordId id);
  bool ReadSaved(int cursor);
  void OnSavedPage(unsigned generation,
                   int cursor,
                   std::optional<base::DictValue> page);
  KrAgentRecordId Id(Names& names, std::string_view key);

  const raw_ref<BrowserWindowInterface> window_;
  const raw_ref<WindowSession> session_;
  std::unique_ptr<krypton_agent::PanelConnection> connection_;
  std::unique_ptr<krypton_agent::ChatCard> saved_card_;
  // Saved-transcript reading state across pages.
  std::vector<krypton_agent::TranscriptEntry> saved_items_;
  std::string saved_reply_id_, saved_reply_;
  std::unique_ptr<krypton_agent::SkillImportPicker> picker_;
  std::optional<krypton_agent::ContextTab> context_tab_;
  PrefChangeRegistrar prefs_;
  gfx::Rect bounds_;
  Names provider_ids_;
  Names session_ids_;
  Names skill_ids_;
  Names site_ids_;
  Names activity_ids_;
  RecordMap providers_;
  RecordMap published_providers_;
  RecordMap sessions_;
  RecordMap skills_;
  RecordMap sites_;
  RecordMap activity_;
  std::map<KrAgentRecordId, Review> reviews_;
  std::set<KrAgentRecordId> published_reviews_;
  KrAgentRecordId next_id_ = 1;
  KrAgentProviderId provider_ = 0;
  KrAgentRecordId shown_id_ = 0;
  unsigned session_generation_ = 0;
  unsigned history_generation_ = 0;
  unsigned transcript_generation_ = 0;
  uint32_t status_ = KR_AGENT_IDLE;
  std::string shown_session_;
  std::string model_;
  std::string title_;
  std::string notice_;
  std::string error_;
  bool visible_ = false;
  bool dark_ = false;
  bool showing_saved_ = false;
  bool saved_pending_ = false;
  base::WeakPtrFactory<AgentPanel> weak_factory_{this};
};

}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_AGENT_PANEL_H_
