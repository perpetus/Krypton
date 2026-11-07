// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_ESSENTIAL_PREFS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_ESSENTIAL_PREFS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class PrefService;
namespace user_prefs {
class PrefRegistrySyncable;
}
namespace krypton_ui {
inline constexpr char kEssentialsPref[] = "krypton_ui.essentials";
struct Essential {
  uint64_t id = 0;
  std::string url;
};
struct EssentialCatalog {
  uint64_t next_id = 1;
  std::vector<Essential> items;
};
void RegisterEssentialPrefs(user_prefs::PrefRegistrySyncable* registry);
EssentialCatalog ReadEssentials(const PrefService& prefs);
std::optional<uint64_t> SaveEssential(PrefService& prefs,
                                      uint64_t id,
                                      std::string url);
bool RemoveEssential(PrefService& prefs, uint64_t id);
}  // namespace krypton_ui
#endif
