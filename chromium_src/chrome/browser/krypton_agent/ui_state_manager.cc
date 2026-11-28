#include "chrome/browser/krypton_agent/ui_state_manager.h"

#include <utility>

#include "chrome/common/actor/action_result.h"

namespace krypton_agent {

void UiStateManager::OnUiEvent(actor::ui::AsyncUiEvent event,
                               actor::ui::UiCompleteCallback callback) {
  std::move(callback).Run(actor::MakeOkResult());
}

void UiStateManager::OnUiEvent(actor::ui::SyncUiEvent event) {}

void UiStateManager::LazyInitTabTracker() {}

base::WeakPtr<actor::ActorNavigationThrottle::Delegate>
UiStateManager::GetNavigationDelegate() {
  return nullptr;
}

}  // namespace krypton_agent
