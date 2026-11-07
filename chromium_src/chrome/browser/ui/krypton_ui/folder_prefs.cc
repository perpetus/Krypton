// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/folder_prefs.h"
#include <algorithm>
#include <limits>
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
namespace krypton_ui {
namespace {
uint64_t Id(const std::string* text) {
  uint64_t id = 0;
  return text && base::StringToUint64(*text, &id) ? id : 0;
}
}  // namespace
const TabFolder* FolderCatalog::Find(KrFolderId id) const {
  const auto found = std::ranges::find(items, id, &TabFolder::id);
  return found == items.end() ? nullptr : &*found;
}
bool FolderCatalog::IsDescendant(KrFolderId id, KrFolderId ancestor) const {
  for (unsigned depth = 0; id && depth < 5; ++depth) {
    if (id == ancestor)
      return true;
    const auto* folder = Find(id);
    id = folder ? folder->parent : 0;
  }
  return false;
}
bool FolderCatalog::Valid() const {
  for (const auto& folder : items) {
    if (!folder.id || !folder.space || folder.name.empty() ||
        folder.name.find_first_not_of(" \t\r\n") == std::string::npos ||
        !base::IsStringUTF8(folder.name) || !base::IsStringUTF8(folder.glyph) ||
        folder.name.find('\0') != std::string::npos ||
        folder.glyph.find('\0') != std::string::npos ||
        std::ranges::count(items, folder.id, &TabFolder::id) != 1)
      return false;
    const TabFolder* current = &folder;
    unsigned depth = 1;
    while (current->parent) {
      current = Find(current->parent);
      if (!current || current->space != folder.space || ++depth > 5)
        return false;
    }
  }
  return true;
}
void RegisterFolderPrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterDictionaryPref(kFoldersPref);
}
FolderCatalog ReadFolders(const PrefService& prefs) {
  FolderCatalog catalog;
  const auto& data = prefs.GetDict(kFoldersPref);
  catalog.next_id = std::max(uint64_t{1}, Id(data.FindString("next_id")));
  if (const auto* items = data.FindList("items")) {
    for (const auto& item : *items) {
      const auto* row = item.GetIfDict();
      if (!row)
        continue;
      const auto id = Id(row->FindString("id"));
      const auto* name = row->FindString("name");
      if (!id || id == std::numeric_limits<KrFolderId>::max() || !name)
        continue;
      TabFolder folder;
      folder.id = id;
      folder.space = Id(row->FindString("space"));
      folder.parent = Id(row->FindString("parent"));
      folder.name = *name;
      if (const auto* glyph = row->FindString("glyph"))
        folder.glyph = *glyph;
      folder.order = Id(row->FindString("order"));
      folder.collapsed = row->FindBool("collapsed").value_or(false);
      catalog.items.push_back(std::move(folder));
      catalog.next_id = std::max(catalog.next_id, id + 1);
    }
  }
  // A corrupt hierarchy must not trap tabs behind an unreachable container.
  if (!catalog.Valid())
    catalog.items.clear();
  return catalog;
}
bool WriteFolders(PrefService& prefs, const FolderCatalog& catalog) {
  if (!catalog.Valid())
    return false;
  base::DictValue data;
  data.Set("next_id", base::NumberToString(catalog.next_id));
  base::ListValue items;
  for (const auto& folder : catalog.items) {
    base::DictValue row;
    row.Set("id", base::NumberToString(folder.id));
    row.Set("space", base::NumberToString(folder.space));
    row.Set("parent", base::NumberToString(folder.parent));
    row.Set("name", folder.name);
    row.Set("glyph", folder.glyph);
    row.Set("order", base::NumberToString(folder.order));
    row.Set("collapsed", folder.collapsed);
    items.Append(std::move(row));
  }
  data.Set("items", std::move(items));
  prefs.SetDict(kFoldersPref, std::move(data));
  return true;
}
}  // namespace krypton_ui
