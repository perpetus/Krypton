#ifndef CHROME_BROWSER_KRYPTON_AGENT_CONFIRMATION_GATE_H_
#define CHROME_BROWSER_KRYPTON_AGENT_CONFIRMATION_GATE_H_

#include <string_view>

#include "base/values.h"
#include "chrome/browser/actor/actor_task_delegate.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace krypton_agent {
struct PageOutline;
struct PageTarget;

class ConfirmationGate final : public actor::ActorTaskDelegate {
 public:
  bool NeedsConfirmation(std::string_view tool,
                         const base::DictValue& arguments,
                         const PageOutline& page,
                         const PageTarget* target) const;
  static bool IsWebNavigation(const GURL& url);
  void FlagPageInstructions() { flagged_ = true; }
  bool requires_confirmation() const { return flagged_; }
  static void LogDenial(std::string_view action);

  void OnTabAddedToTask(actor::TaskId, const tabs::TabHandle&) override;
  void OnTaskTabsVisibilityChanged(actor::TaskId, bool) override;
  void RequestToShowCredentialSelectionDialog(
      actor::TaskId,
      const base::flat_map<std::string, gfx::Image>&,
      const std::vector<actor_login::Credential>&,
      CredentialSelectedCallback callback) override;
  void RequestToShowUserConfirmationDialog(
      actor::TaskId,
      const url::Origin&,
      bool,
      UserConfirmationDialogCallback callback) override;
  void RequestToConfirmNavigation(actor::TaskId,
                                  const url::Origin&,
                                  NavigationConfirmationCallback) override;
  void RequestToShowAutofillSuggestionsDialog(
      actor::TaskId,
      std::vector<autofill::ActorFormFillingRequest>,
      base::WeakPtr<actor::AutofillSelectionDialogEventHandler>,
      AutofillSuggestionSelectedCallback callback) override;
  void RequestToShowGmailOtpOptInDialog(
      actor::TaskId,
      GmailOtpOptInCallback callback) override;
  void RequestToShowGmailOtpConfirmationDialog(
      actor::TaskId,
      const std::string&,
      GmailOtpConfirmationCallback callback) override;

  base::WeakPtr<actor::ActorTaskDelegate> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  bool flagged_ = false;
  base::WeakPtrFactory<ConfirmationGate> weak_factory_{this};
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_CONFIRMATION_GATE_H_
