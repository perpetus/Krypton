#include "chrome/browser/krypton_agent/site_allowlist.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "base/strings/strcat.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace krypton_agent {
namespace {

constexpr size_t kMaxSites = 256;
constexpr char kKnownUserContent[] = "krypton.agent.user_content_sites";

std::string Site(const GURL& url) {
  return net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

std::optional<std::string> ParseEntry(std::string_view entry) {
  if (entry.empty() || entry.size() > 512) {
    return std::nullopt;
  }
  const bool exact = entry.starts_with("https://");
  const GURL url(exact ? std::string(entry)
                       : base::StrCat({"https://", entry}));
  if (!url.is_valid() || !url.SchemeIs(url::kHttpsScheme) || !url.has_host() ||
      url.has_username() || url.has_password() || url.has_query() ||
      url.has_ref() || url.path() != "/" || url.host().ends_with('.')) {
    return std::nullopt;
  }
  if (exact) {
    return url::Origin::Create(url).Serialize();
  }
  if (url.has_port() || entry.ends_with('/') || Site(url) != url.host()) {
    return std::nullopt;
  }
  return url.GetHost();
}

}  // namespace

SiteEntryStatus GetAgentSiteEntryStatus(const Profile& profile,
                                        std::string_view entry) {
  const auto parsed = ParseEntry(entry);
  if (!parsed || !profile.IsRegularProfile()) {
    return {};
  }
  const GURL url(parsed->starts_with("https://")
                     ? *parsed
                     : base::StrCat({"https://", *parsed}));
  return {.valid = true, .user_content = IsKnownUserContentSite(profile, url)};
}

bool SiteHasUserContent(const GURL& url) {
  static constexpr std::array<std::string_view, 11> sites{
      "discord.com", "facebook.com",      "github.com", "instagram.com",
      "reddit.com",  "stackoverflow.com", "tiktok.com", "wikipedia.org",
      "x.com",       "youtube.com",       "gitlab.com"};
  return std::ranges::contains(sites, Site(url));
}

bool IsAllowlisted(const Profile& profile, const GURL& url) {
  if (!profile.IsRegularProfile() || !url.SchemeIs(url::kHttpsScheme) ||
      !url.is_valid() || url.has_username() || url.has_password()) {
    return false;
  }
  const auto origin = url::Origin::Create(url).Serialize();
  const auto site = Site(url);
  const auto& entries = profile.GetPrefs()->GetList(kSiteAllowlist);
  return std::ranges::any_of(entries, [&](const base::Value& value) {
    const auto* text = value.GetIfString();
    const auto entry = text ? ParseEntry(*text) : std::nullopt;
    return entry && (*entry == origin || (!site.empty() && *entry == site));
  });
}

base::DictValue GetAgentSiteAllowlist(Profile& profile) {
  base::ListValue entries;
  if (!profile.IsRegularProfile() || !IsProductionAgentEnabled()) {
    return base::DictValue().Set("entries", std::move(entries));
  }
  const auto& prefs = *profile.GetPrefs();
  for (const auto& value : prefs.GetList(kSiteAllowlist)) {
    if (entries.size() == kMaxSites) {
      break;
    }
    const auto* text = value.GetIfString();
    const auto entry = text ? ParseEntry(*text) : std::nullopt;
    if (!entry) {
      continue;
    }
    const GURL url(entry->starts_with("https://")
                       ? *entry
                       : base::StrCat({"https://", *entry}));
    const bool user_content = IsKnownUserContentSite(profile, url);
    auto item = base::DictValue()
                    .Set("entry", *entry)
                    .Set("user_content", user_content);
    if (user_content) {
      item.Set("warning", kUserContentWarning);
    }
    entries.Append(std::move(item));
  }
  return base::DictValue()
      .Set("entries", std::move(entries))
      .Set("managed", prefs.IsManagedPreference(kSiteAllowlist));
}

bool SetAgentSiteAllowlistEntry(Profile& profile,
                                std::string_view entry,
                                bool allowed,
                                bool acknowledged_user_content) {
  auto parsed = ParseEntry(entry);
  if (!parsed || !profile.IsRegularProfile() || !IsProductionAgentEnabled() ||
      profile.GetPrefs()->IsManagedPreference(kSiteAllowlist)) {
    return false;
  }
  const GURL url(parsed->starts_with("https://")
                     ? *parsed
                     : base::StrCat({"https://", *parsed}));
  if (allowed && IsKnownUserContentSite(profile, url) &&
      !acknowledged_user_content) {
    return false;
  }
  ScopedListPrefUpdate update(profile.GetPrefs(), kSiteAllowlist);
  if (allowed) {
    if (!update->contains(*parsed)) {
      if (update->size() >= kMaxSites) {
        return false;
      }
      update->Append(*parsed);
    }
  } else {
    update->EraseValue(base::Value(*parsed));
  }
  return true;
}

void MarkSiteWithUserContent(Profile& profile, const GURL& url) {
  auto site = Site(url);
  if (site.empty() || !profile.IsRegularProfile() ||
      !IsProductionAgentEnabled()) {
    return;
  }
  auto* prefs = profile.GetPrefs();
  const auto& sites = prefs->GetList(kKnownUserContent);
  if (sites.size() >= kMaxSites || sites.contains(site)) {
    return;
  }
  ScopedListPrefUpdate(prefs, kKnownUserContent)->Append(std::move(site));
}

bool IsKnownUserContentSite(const Profile& profile, const GURL& url) {
  return profile.IsRegularProfile() &&
         (SiteHasUserContent(url) ||
          profile.GetPrefs()->GetList(kKnownUserContent).contains(Site(url)));
}

}  // namespace krypton_agent
