#ifndef COMPONENTS_KRYPTON_BRANDING_PERSISTED_NAMES_H_
#define COMPONENTS_KRYPTON_BRANDING_PERSISTED_NAMES_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"

class PrefService;

namespace krypton::branding {

// Only metadata explicitly marked as browser-generated may be migrated.
// Non-Latin prefixes and grammatical suffixes are retained around the brand.
std::optional<std::string> RebrandDefaultProfileName(std::string_view name,
                                                     bool is_default);
bool MigrateDefaultProfileName(PrefService& prefs,
                               std::string_view name_pref,
                               std::string_view default_pref);
bool MigrateCachedDefaultProfileName(base::DictValue& attributes,
                                     std::string_view name_key,
                                     std::string_view default_key);
bool ShouldRefreshDefaultProfileShortcut(std::u16string_view old_name,
                                         std::u16string_view current_name,
                                         bool is_default);

}  // namespace krypton::branding
#endif
