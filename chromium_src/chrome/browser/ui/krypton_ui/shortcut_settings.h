// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SHORTCUT_SETTINGS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SHORTCUT_SETTINGS_H_

#include <vector>

#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"

class PrefService;
struct AcceleratorMapping;

namespace krypton_ui {

enum ShortcutCommand {
  kShortcutSplitGrid = 0x4b520001,
  kShortcutSplitColumns = 0x4b520002,
  kShortcutSplitRows = 0x4b520003,
  kShortcutUnsplit = 0x4b520004,
  kShortcutSleepTab = 0x4b520005,
  kShortcutNewNote = 0x4b520006,
  kShortcutNewEasel = 0x4b520007,
};

void ApplyShortcutOverrides(std::vector<AcceleratorMapping>& mappings,
                            const PrefService& local_state,
                            const PrefService& profile_prefs,
                            bool has_native_shell);

void PublishShortcutCatalog(const KrShellUI& ui,
                            const PrefService& local_state,
                            const PrefService& profile_prefs);
KrShortcutEditResult SetShortcut(PrefService& local_state,
                                 const PrefService& profile_prefs,
                                 uint32_t command,
                                 KrShortcutChord chord,
                                 uint32_t replace_conflict);
KrShortcutEditResult UnassignShortcut(PrefService& local_state,
                                      const PrefService& profile_prefs,
                                      uint32_t command);
KrShortcutEditResult ResetShortcut(PrefService& local_state,
                                   const PrefService& profile_prefs,
                                   uint32_t command);

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_SHORTCUT_SETTINGS_H_
