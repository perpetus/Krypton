// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_FOLDER_PREFS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_FOLDER_PREFS_H_
#include <string>
#include <vector>
#include "krypton_ui/abi.h"
class PrefService;
namespace user_prefs {
class PrefRegistrySyncable;
}
namespace krypton_ui {
inline constexpr char kFoldersPref[] = "krypton_ui.folders";
struct TabFolder {
  KrFolderId id = 0;
  KrSpaceId space = 0;
  KrFolderId parent = 0;
  std::string name, glyph;
  size_t order = 0;
  bool collapsed = false;
  bool operator==(const TabFolder&) const = default;
};
struct FolderCatalog {
  std::vector<TabFolder> items;
  KrFolderId next_id = 1;
  const TabFolder* Find(KrFolderId id) const;
  bool IsDescendant(KrFolderId id, KrFolderId ancestor) const;
  bool Valid() const;
};
void RegisterFolderPrefs(user_prefs::PrefRegistrySyncable* registry);
FolderCatalog ReadFolders(const PrefService& prefs);
bool WriteFolders(PrefService& prefs, const FolderCatalog& catalog);
}  // namespace krypton_ui
#endif
