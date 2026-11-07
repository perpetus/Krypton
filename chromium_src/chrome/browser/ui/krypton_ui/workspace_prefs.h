// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_PREFS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_PREFS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "krypton_ui/abi.h"

class PrefService;
namespace user_prefs {
class PrefRegistrySyncable;
}

namespace krypton_ui {
inline constexpr char kWorkspacesPref[] = "krypton_ui.workspaces";
inline constexpr char kLastWorkspacePref[] = "krypton_ui.last_workspace";
inline constexpr KrSpaceId kDefaultWorkspace = 1;

struct Workspace {
  KrSpaceId id = 0;
  std::string name;
  std::string glyph;
  KrColor color = 0;
};
struct WorkspaceCatalog {
  std::vector<Workspace> spaces;
  KrSpaceId next_id = 2;
  [[nodiscard]] bool Contains(KrSpaceId id) const;
};

void RegisterWorkspacePrefs(user_prefs::PrefRegistrySyncable* registry);
[[nodiscard]] WorkspaceCatalog ReadWorkspaces(const PrefService& prefs);
// Returns the assigned id. Zero and unknown ids are rejected except on create.
[[nodiscard]] std::optional<KrSpaceId> SaveWorkspace(PrefService& prefs,
                                                     Workspace value);
// The default workspace cannot be deleted. Tabs are retained by the host.
[[nodiscard]] bool RemoveWorkspace(PrefService& prefs, KrSpaceId id);
[[nodiscard]] KrSpaceId LastWorkspace(const PrefService& prefs,
                                      const WorkspaceCatalog& catalog);
void RememberWorkspace(PrefService& prefs, KrSpaceId id);
}  // namespace krypton_ui
#endif
