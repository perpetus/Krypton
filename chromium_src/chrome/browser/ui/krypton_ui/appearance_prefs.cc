#include "chrome/browser/ui/krypton_ui/layout_prefs.h"

#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"

namespace krypton_ui {
void RegisterAppearanceProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterIntegerPref(kColorSchemePref, KR_COLOR_SYSTEM);
  registry->RegisterIntegerPref(kMenuMaterialPref, KR_MENU_MATERIAL_DEFAULT);
  registry->RegisterIntegerPref(kAppearanceFlagsPref, 0);
  registry->RegisterIntegerPref(kIconPackPref, KR_ICON_PACK_FLUENT);
  registry->RegisterIntegerPref(kGlassModePref, KR_GLASS_OFF);
}
KrAppearancePrefs ReadAppearancePrefs(const PrefService& prefs) {
  const auto bounded = [&](const char* name, int maximum) -> uint32_t {
    const int value = prefs.GetInteger(name);
    return value >= 0 && value <= maximum ? static_cast<uint32_t>(value) : 0u;
  };
  return {sizeof(KrAppearancePrefs),
          bounded(kColorSchemePref, KR_COLOR_DARK),
          bounded(kMenuMaterialPref, KR_MENU_MATERIAL_ACRYLIC_THIN),
          static_cast<uint32_t>(prefs.GetInteger(kAppearanceFlagsPref)),
          bounded(kIconPackPref, KR_ICON_PACK_RADIX),
          bounded(kGlassModePref, KR_GLASS_QUALITY)};
}
bool WriteAppearancePrefs(PrefService& prefs,
                          const KrAppearancePrefs* appearance) {
  if (!appearance || !KR_HAS_FIELD(appearance, KrAppearancePrefs, flags))
    return false;
  const auto icon_pack = KR_HAS_FIELD(appearance, KrAppearancePrefs, icon_pack)
                             ? appearance->icon_pack
                             : KR_ICON_PACK_FLUENT;
  const auto glass_mode =
      KR_HAS_FIELD(appearance, KrAppearancePrefs, glass_mode)
          ? appearance->glass_mode
          : KR_GLASS_OFF;
  if (appearance->color_scheme > KR_COLOR_DARK ||
      appearance->menu_material > KR_MENU_MATERIAL_ACRYLIC_THIN ||
      icon_pack > KR_ICON_PACK_RADIX || glass_mode > KR_GLASS_QUALITY)
    return false;
  prefs.SetInteger(kColorSchemePref,
                   static_cast<int>(appearance->color_scheme));
  prefs.SetInteger(kMenuMaterialPref,
                   static_cast<int>(appearance->menu_material));
  prefs.SetInteger(kAppearanceFlagsPref, static_cast<int>(appearance->flags));
  prefs.SetInteger(kIconPackPref, static_cast<int>(icon_pack));
  prefs.SetInteger(kGlassModePref, static_cast<int>(glass_mode));
  return true;
}
}  // namespace krypton_ui
