#ifndef CHROME_BROWSER_KRYPTON_AGENT_SITE_ALLOWLIST_H_
#define CHROME_BROWSER_KRYPTON_AGENT_SITE_ALLOWLIST_H_

#include <string_view>

#include "base/values.h"

class GURL;
class Profile;

namespace krypton_agent {

inline constexpr char kSiteAllowlist[] = "krypton.agent.site_allowlist";
inline constexpr char kUserContentWarning[] =
    "Pages on this site include content written by other people, which can "
    "contain hidden instructions for the agent. Allowing it lets the agent "
    "open its pages without asking you.";

struct SiteEntryStatus {
  bool valid = false;
  bool user_content = false;
};
SiteEntryStatus GetAgentSiteEntryStatus(const Profile& profile,
                                        std::string_view entry);

bool IsAllowlisted(const Profile& profile, const GURL& url);
bool SiteHasUserContent(const GURL& url);
base::DictValue GetAgentSiteAllowlist(Profile& profile);
bool SetAgentSiteAllowlistEntry(Profile& profile,
                                std::string_view entry,
                                bool allowed,
                                bool acknowledged_user_content);
void MarkSiteWithUserContent(Profile& profile, const GURL& url);
bool IsKnownUserContentSite(const Profile& profile, const GURL& url);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_SITE_ALLOWLIST_H_
