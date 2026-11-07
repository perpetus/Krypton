// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_THEME_PREFS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_THEME_PREFS_H_

#include <optional>

#include "chrome/browser/ui/krypton_ui/workspace_prefs.h"

namespace krypton_ui {
inline constexpr char kWorkspaceThemesPref[] = "krypton_ui.workspace_themes";

void RegisterWorkspaceThemePrefs(user_prefs::PrefRegistrySyncable* registry);
[[nodiscard]] bool ValidWorkspaceTheme(const KrWorkspaceTheme& theme);
[[nodiscard]] KrWorkspaceTheme ReadWorkspaceTheme(const PrefService& prefs,
                                                  KrSpaceId space,
                                                  uint32_t scheme);
[[nodiscard]] bool SaveWorkspaceTheme(PrefService& prefs,
                                      const KrWorkspaceTheme& theme);
void RemoveWorkspaceTheme(PrefService& prefs, KrSpaceId space);

// Drafts belong to one window. Only a committed draft enters profile prefs;
// other windows retain their own drafts until their editors are dismissed.
class WorkspaceThemeState {
 public:
  [[nodiscard]] bool Apply(PrefService& prefs,
                           const KrWorkspaceTheme& theme,
                           bool commit);
  [[nodiscard]] KrWorkspaceTheme Resolve(const PrefService& prefs,
                                         KrSpaceId space,
                                         uint32_t scheme) const;
  [[nodiscard]] std::optional<uint32_t> preview_scheme() const;
  [[nodiscard]] const KrWorkspaceTheme* preview() const;
  void Retain(const WorkspaceCatalog& catalog);

 private:
  std::optional<KrWorkspaceTheme> preview_;
};
}  // namespace krypton_ui
#endif
