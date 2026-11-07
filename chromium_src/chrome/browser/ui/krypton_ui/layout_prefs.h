// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_LAYOUT_PREFS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_LAYOUT_PREFS_H_

class PrefService;
struct KrLayoutPrefs;
struct KrAppearancePrefs;

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace krypton_ui {

// The shell layout belongs to the profile, so every window shows the same one.
inline constexpr char kLayoutFlagsPref[] = "krypton_ui.layout_flags";
inline constexpr char kSidebarWidthPref[] = "krypton_ui.sidebar_width";
inline constexpr char kColorSchemePref[] = "krypton_ui.appearance.color_scheme";
inline constexpr char kMenuMaterialPref[] =
    "krypton_ui.appearance.menu_material";
inline constexpr char kAppearanceFlagsPref[] = "krypton_ui.appearance.flags";
inline constexpr char kIconPackPref[] = "krypton_ui.appearance.icon_pack";
inline constexpr char kGlassModePref[] = "krypton_ui.appearance.glass_mode";
inline constexpr char kLibraryMediaEnabledPref[] =
    "krypton_ui.library.media_enabled";

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);
void RegisterAppearanceProfilePrefs(user_prefs::PrefRegistrySyncable* registry);
KrLayoutPrefs ReadLayoutPrefs(const PrefService& prefs);
void WriteLayoutPrefs(PrefService& prefs, const KrLayoutPrefs& layout);
KrAppearancePrefs ReadAppearancePrefs(const PrefService& prefs);
bool WriteAppearancePrefs(PrefService& prefs,
                          const KrAppearancePrefs* appearance);

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_LAYOUT_PREFS_H_
