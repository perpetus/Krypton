// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/workspace_prefs.h"
#include "chrome/browser/ui/krypton_ui/folder_prefs.h"
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "chrome/browser/ui/krypton_ui/workspace_theme_prefs.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"

namespace krypton_ui {
namespace {
KrSpaceId ReadId(const std::string* text) {
  uint64_t id = 0;
  return text && base::StringToUint64(*text, &id) ? id : 0;
}
bool ValidText(std::string_view text) {
  return text.find('\0') == std::string_view::npos && base::IsStringUTF8(text);
}
void Write(PrefService& prefs, const WorkspaceCatalog& catalog) {
  base::DictValue data;
  data.Set("version", 1);
  data.Set("next_id", base::NumberToString(catalog.next_id));
  base::ListValue spaces;
  for (const auto& space : catalog.spaces) {
    base::DictValue row;
    row.Set("id", base::NumberToString(space.id));
    row.Set("name", space.name);
    row.Set("glyph", space.glyph);
    row.Set("color", base::NumberToString(space.color));
    spaces.Append(std::move(row));
  }
  data.Set("spaces", std::move(spaces));
  prefs.SetDict(kWorkspacesPref, std::move(data));
}
}  // namespace

bool WorkspaceCatalog::Contains(KrSpaceId id) const {
  return std::ranges::any_of(
      spaces, [id](const auto& space) { return space.id == id; });
}
void RegisterWorkspacePrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterDictionaryPref(kWorkspacesPref);
  RegisterSiteCustomizationPrefs(registry);
  registry->RegisterStringPref(kLastWorkspacePref, "1");
  RegisterWorkspaceThemePrefs(registry);
  RegisterFolderPrefs(registry);
}
WorkspaceCatalog ReadWorkspaces(const PrefService& prefs) {
  WorkspaceCatalog catalog{{{kDefaultWorkspace, "Default", {}, 0}}};
  const auto& data = prefs.GetDict(kWorkspacesPref);
  if (data.FindInt("version").value_or(1) != 1)
    return catalog;
  catalog.next_id = std::max(KrSpaceId{2}, ReadId(data.FindString("next_id")));
  if (const auto* rows = data.FindList("spaces")) {
    for (const auto& row : *rows) {
      const auto* value = row.GetIfDict();
      if (!value)
        continue;
      const auto id = ReadId(value->FindString("id"));
      if (!id || id == std::numeric_limits<KrSpaceId>::max())
        continue;
      // Even a damaged label must not let a future workspace reuse this id
      // and acquire tabs restored from the old workspace.
      catalog.next_id = std::max(catalog.next_id, id + 1);
      const auto* name = value->FindString("name");
      const auto* glyph = value->FindString("glyph");
      if (!name || !ValidText(*name) || !glyph || !ValidText(*glyph))
        continue;
      std::string trimmed;
      base::TrimWhitespaceASCII(*name, base::TRIM_ALL, &trimmed);
      if (trimmed.empty())
        continue;
      uint32_t color = 0;
      if (const auto* text = value->FindString("color");
          text && !base::StringToUint(*text, &color))
        color = 0;
      Workspace space{id, std::move(trimmed), *glyph, color};
      if (id == kDefaultWorkspace) {
        catalog.spaces.front() = std::move(space);
      } else if (!catalog.Contains(id)) {
        catalog.spaces.push_back(std::move(space));
      }
    }
  }
  return catalog;
}
std::optional<KrSpaceId> SaveWorkspace(PrefService& prefs, Workspace value) {
  if (!ValidText(value.name) || !ValidText(value.glyph))
    return std::nullopt;
  base::TrimWhitespaceASCII(value.name, base::TRIM_ALL, &value.name);
  if (value.name.empty())
    return std::nullopt;
  auto catalog = ReadWorkspaces(prefs);
  if (!value.id) {
    if (catalog.next_id == std::numeric_limits<KrSpaceId>::max())
      return std::nullopt;
    value.id = catalog.next_id++;
    catalog.spaces.push_back(value);
  } else {
    const auto found =
        std::ranges::find(catalog.spaces, value.id, &Workspace::id);
    if (found == catalog.spaces.end())
      return std::nullopt;
    *found = value;
  }
  Write(prefs, catalog);
  return value.id;
}
bool RemoveWorkspace(PrefService& prefs, KrSpaceId id) {
  auto catalog = ReadWorkspaces(prefs);
  if (id == kDefaultWorkspace || !catalog.Contains(id))
    return false;
  std::erase_if(catalog.spaces,
                [id](const auto& space) { return space.id == id; });
  Write(prefs, catalog);
  RemoveWorkspaceTheme(prefs, id);
  return true;
}
KrSpaceId LastWorkspace(const PrefService& prefs,
                        const WorkspaceCatalog& catalog) {
  const auto id = ReadId(&prefs.GetString(kLastWorkspacePref));
  return catalog.Contains(id) ? id : kDefaultWorkspace;
}
void RememberWorkspace(PrefService& prefs, KrSpaceId id) {
  prefs.SetString(kLastWorkspacePref, base::NumberToString(id));
}
}  // namespace krypton_ui
