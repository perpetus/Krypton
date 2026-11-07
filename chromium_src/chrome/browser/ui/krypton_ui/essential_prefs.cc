// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/essential_prefs.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {
uint64_t Parse(const std::string* text) {
  uint64_t id = 0;
  return text && base::StringToUint64(*text, &id) ? id : 0;
}
void Write(PrefService& prefs, const EssentialCatalog& catalog) {
  base::DictValue data;
  data.Set("next_id", base::NumberToString(catalog.next_id));
  base::ListValue items;
  for (const auto& item : catalog.items) {
    base::DictValue value;
    value.Set("id", base::NumberToString(item.id));
    value.Set("url", item.url);
    items.Append(std::move(value));
  }
  data.Set("items", std::move(items));
  prefs.SetDict(kEssentialsPref, std::move(data));
}
}  // namespace
void RegisterEssentialPrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterDictionaryPref(kEssentialsPref);
}
EssentialCatalog ReadEssentials(const PrefService& prefs) {
  EssentialCatalog catalog;
  const auto& data = prefs.GetDict(kEssentialsPref);
  const auto* next_text = data.FindString("next_id");
  if (next_text)
    catalog.next_id = Parse(next_text);
  const auto* items = data.FindList("items");
  if (!items)
    return catalog;
  uint64_t highest = 0;
  for (const auto& value : *items) {
    const auto* row = value.GetIfDict();
    if (!row)
      continue;
    const auto id = Parse(row->FindString("id"));
    highest = std::max(highest, id);
    const auto* url = row->FindString("url");
    if (id && url && GURL(*url).is_valid() &&
        std::ranges::none_of(catalog.items,
                             [id](const auto& item) { return item.id == id; }))
      catalog.items.push_back({id, *url});
  }
  if (highest == std::numeric_limits<uint64_t>::max())
    catalog.next_id = 0;
  else if (catalog.next_id)
    catalog.next_id = std::max(catalog.next_id, highest + 1);
  return catalog;
}
std::optional<uint64_t> SaveEssential(PrefService& prefs,
                                      uint64_t id,
                                      std::string url) {
  if (!GURL(url).is_valid())
    return std::nullopt;
  auto catalog = ReadEssentials(prefs);
  if (id) {
    const auto found = std::ranges::find(catalog.items, id, &Essential::id);
    if (found == catalog.items.end())
      return std::nullopt;
    if (found->url == url)
      return id;
    found->url = std::move(url);
  } else {
    id = catalog.next_id;
    if (!id)
      return std::nullopt;
    catalog.next_id = id == std::numeric_limits<uint64_t>::max() ? 0 : id + 1;
    catalog.items.push_back({id, std::move(url)});
  }
  Write(prefs, catalog);
  return id;
}
bool RemoveEssential(PrefService& prefs, uint64_t id) {
  auto catalog = ReadEssentials(prefs);
  if (!std::erase_if(catalog.items,
                     [id](const auto& item) { return item.id == id; }))
    return false;
  Write(prefs, catalog);
  return true;
}
}  // namespace krypton_ui
