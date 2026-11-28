#ifndef CHROME_BROWSER_KRYPTON_AGENT_UI_STATE_MANAGER_H_
#define CHROME_BROWSER_KRYPTON_AGENT_UI_STATE_MANAGER_H_

#include "chrome/browser/actor/ui/actor_ui_state_manager_interface.h"

namespace krypton_agent {

class UiStateManager final : public actor::ui::ActorUiStateManagerInterface {
 public:
  void OnUiEvent(actor::ui::AsyncUiEvent event,
                 actor::ui::UiCompleteCallback callback) override;
  void OnUiEvent(actor::ui::SyncUiEvent event) override;
  void LazyInitTabTracker() override;
  base::WeakPtr<actor::ActorNavigationThrottle::Delegate>
  GetNavigationDelegate() override;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_UI_STATE_MANAGER_H_
