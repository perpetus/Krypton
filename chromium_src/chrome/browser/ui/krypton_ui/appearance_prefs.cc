#include "chrome/browser/ui/krypton_ui/layout_prefs.h"

#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"

namespace krypton_ui {
namespace {

constexpr uint32_t kAgentChatFlags =
    KR_APPEARANCE_AGENT_CHAT_AVAILABLE | KR_APPEARANCE_NEW_TAB_AGENT_CHAT;
constexpr uint32_t kNewTabPage = KR_APPEARANCE_NEW_TAB_PAGE;

}  // namespace
void RegisterAppearanceProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterIntegerPref(kColorSchemePref, KR_COLOR_SYSTEM);
  registry->RegisterIntegerPref(kMenuMaterialPref, KR_MENU_MATERIAL_DEFAULT);
  registry->RegisterIntegerPref(kAppearanceFlagsPref, 0);
  registry->RegisterBooleanPref(kAgentChatNewTabOverridePref, false);
  registry->RegisterIntegerPref(kIconPackPref, KR_ICON_PACK_FLUENT);
  // Retain the stored key for compatibility. Liquid Glass is disabled
  // for this release; reads and writes intentionally use KR_GLASS_OFF.
  registry->RegisterIntegerPref(kGlassModePref, KR_GLASS_OFF);
}
KrAppearancePrefs ReadAppearancePrefs(const PrefService& prefs,
                                      bool agent_enabled) {
  const auto bounded = [&](const char* name, int maximum) -> uint32_t {
    const int value = prefs.GetInteger(name);
    return value >= 0 && value <= maximum ? static_cast<uint32_t>(value) : 0u;
  };
  auto flags = static_cast<uint32_t>(prefs.GetInteger(kAppearanceFlagsPref));
  flags &= ~kAgentChatFlags;
  if (agent_enabled) {
    flags |= KR_APPEARANCE_AGENT_CHAT_AVAILABLE;
    if (!prefs.GetBoolean(kAgentChatNewTabOverridePref)) {
      flags &= ~kNewTabPage;
      flags |= KR_APPEARANCE_NEW_TAB_AGENT_CHAT;
    }
  }
  return {sizeof(KrAppearancePrefs),
          bounded(kColorSchemePref, KR_COLOR_DARK),
          bounded(kMenuMaterialPref, KR_MENU_MATERIAL_ACRYLIC_THIN),
          flags,
          bounded(kIconPackPref, KR_ICON_PACK_RADIX),
          KR_GLASS_OFF};
}
bool WriteAppearancePrefs(PrefService& prefs,
                          const KrAppearancePrefs* appearance,
                          bool agent_enabled) {
  if (!appearance || !KR_HAS_FIELD(appearance, KrAppearancePrefs, flags)) {
    return false;
  }
  const auto icon_pack = KR_HAS_FIELD(appearance, KrAppearancePrefs, icon_pack)
                             ? appearance->icon_pack
                             : KR_ICON_PACK_FLUENT;
  const auto glass_mode =
      KR_HAS_FIELD(appearance, KrAppearancePrefs, glass_mode)
          ? appearance->glass_mode
          : KR_GLASS_OFF;
  if (appearance->color_scheme > KR_COLOR_DARK ||
      appearance->menu_material > KR_MENU_MATERIAL_ACRYLIC_THIN ||
      icon_pack > KR_ICON_PACK_RADIX || glass_mode > KR_GLASS_QUALITY) {
    return false;
  }
  prefs.SetInteger(kColorSchemePref,
                   static_cast<int>(appearance->color_scheme));
  prefs.SetInteger(kMenuMaterialPref,
                   static_cast<int>(appearance->menu_material));
  auto flags = appearance->flags & ~kAgentChatFlags;
  const bool supports_chat =
      (appearance->flags & KR_APPEARANCE_AGENT_CHAT_AVAILABLE) != 0;
  const bool wants_chat =
      (appearance->flags & kAgentChatFlags) == kAgentChatFlags;
  if (agent_enabled && supports_chat) {
    prefs.SetBoolean(kAgentChatNewTabOverridePref, !wants_chat);
  }
  if (wants_chat) {
    const auto previous =
        static_cast<uint32_t>(prefs.GetInteger(kAppearanceFlagsPref));
    flags = (flags & ~kNewTabPage) | (previous & kNewTabPage);
  }
  prefs.SetInteger(kAppearanceFlagsPref, static_cast<int>(flags));
  prefs.SetInteger(kIconPackPref, static_cast<int>(icon_pack));
  prefs.SetInteger(kGlassModePref, KR_GLASS_OFF);
  return true;
}
}  // namespace krypton_ui
