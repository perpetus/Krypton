#ifndef CHROME_BROWSER_KRYPTON_AGENT_HISTORY_PREFS_H_
#define CHROME_BROWSER_KRYPTON_AGENT_HISTORY_PREFS_H_

class PrefRegistrySimple;
class PrefService;

namespace krypton_agent {

inline constexpr char kHistoryEnabled[] = "krypton.agent.history.enabled";
inline constexpr char kArchiveAfterDays[] =
    "krypton.agent.history.archive_after_days";
inline constexpr char kDeleteAfterDays[] =
    "krypton.agent.history.delete_after_days";
inline constexpr char kProviderConsents[] = "krypton.agent.provider_consents";
inline constexpr char kNavigationPermission[] =
    "krypton.agent.navigation_permission";

enum class NavigationPermission { kAsk = 1, kAllow = 2, kBlock = 3 };

NavigationPermission GetNavigationPermission(const PrefService& prefs);

void RegisterHistoryPrefs(PrefRegistrySimple* registry);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_HISTORY_PREFS_H_
