#include "components/krypton_branding/persisted_names.h"

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/prefs/pref_service.h"

namespace krypton::branding {

std::optional<std::string> RebrandDefaultProfileName(std::string_view name,
                                                     bool is_default) {
  if (!is_default || name.find("Helium") == std::string_view::npos)
    return std::nullopt;
  std::string branded(name);
  base::ReplaceSubstringsAfterOffset(&branded, 0, "Helium", "Krypton");
  return branded;
}

bool MigrateDefaultProfileName(PrefService& prefs,
                               std::string_view name_pref,
                               std::string_view default_pref) {
  const auto* name = prefs.FindPreference(name_pref);
  const auto* is_default = prefs.FindPreference(default_pref);
  if (!name || !is_default || name->IsManaged() ||
      name->IsManagedByCustodian() || name->IsRecommended() ||
      name->IsExtensionControlled() || !name->GetValue()->is_string() ||
      !is_default->GetValue()->is_bool())
    return false;
  const auto branded = RebrandDefaultProfileName(
      prefs.GetString(name_pref), prefs.GetBoolean(default_pref));
  if (!branded)
    return false;
  prefs.SetString(name_pref, *branded);
  return true;
}

bool MigrateCachedDefaultProfileName(base::DictValue& attributes,
                                     std::string_view name_key,
                                     std::string_view default_key) {
  const auto* name = attributes.FindString(name_key);
  if (!name)
    return false;
  const auto branded = RebrandDefaultProfileName(
      *name, attributes.FindBool(default_key).value_or(false));
  if (!branded)
    return false;
  attributes.Set(name_key, *branded);
  return true;
}

bool ShouldRefreshDefaultProfileShortcut(std::u16string_view old_name,
                                         std::u16string_view current_name,
                                         bool is_default) {
  if (!is_default || old_name.find(u"Helium") == std::u16string_view::npos)
    return false;
  const auto branded =
      RebrandDefaultProfileName(base::UTF16ToUTF8(old_name), is_default);
  return branded && base::UTF8ToUTF16(*branded) == current_name;
}

}  // namespace krypton::branding
