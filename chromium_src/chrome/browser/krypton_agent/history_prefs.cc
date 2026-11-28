#include "chrome/browser/krypton_agent/history_prefs.h"

#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace krypton_agent {

NavigationPermission GetNavigationPermission(const PrefService& prefs) {
  switch (prefs.GetInteger(kNavigationPermission)) {
    case static_cast<int>(NavigationPermission::kAllow):
      return NavigationPermission::kAllow;
    case static_cast<int>(NavigationPermission::kBlock):
      return NavigationPermission::kBlock;
    default:
      return NavigationPermission::kAsk;
  }
}

void RegisterHistoryPrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kHistoryEnabled, true);
  registry->RegisterIntegerPref(kArchiveAfterDays, 7);
  registry->RegisterIntegerPref(kDeleteAfterDays, 30);
  registry->RegisterDictionaryPref(kProviderConsents);
  registry->RegisterIntegerPref(kNavigationPermission,
                                static_cast<int>(NavigationPermission::kAsk));
  registry->RegisterListPref("krypton.agent.site_allowlist");
  registry->RegisterListPref("krypton.agent.user_content_sites");
}

}  // namespace krypton_agent
