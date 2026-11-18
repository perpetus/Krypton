// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_PREFS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_PREFS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
// Space id -> {"profile": profile directory base name}. A separate preference:
// builds that predate it rewrite catalog rows without erasing the mapping.
inline constexpr char kWorkspaceProfilesPref[] =
    "krypton_ui.workspace_profiles";
// Never a valid base name. A damaged mapping stays unusable instead of being
// read as the catalog owner's profile.
inline constexpr char kDamagedWorkspaceProfile[] = "/";

struct Workspace {
  KrSpaceId id = 0;
  std::string name;
  std::string glyph;
  KrColor color = 0;
  // Base name of the profile directory that hosts this Space's tabs, the key
  // ProfileAttributesStorage uses. Empty means the profile that owns this
  // catalog. Catalog edits never change it; see SetWorkspaceProfile().
  std::string profile;
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
// A single, bounded profile directory component. Syntax only: existence and
// eligibility are resolved against ProfileAttributesStorage by the host.
[[nodiscard]] bool ValidProfileBaseName(std::string_view name);
// Stores the profile mapping of an existing Space; empty restores the catalog
// owner. The Default Space always stays with the owner. Callers validate the
// profile and refuse Spaces that still hold tabs or folders.
[[nodiscard]] bool SetWorkspaceProfile(PrefService& prefs,
                                       KrSpaceId id,
                                       std::string_view profile);
}  // namespace krypton_ui
#endif
