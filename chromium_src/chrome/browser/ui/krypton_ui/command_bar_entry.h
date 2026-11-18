// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_COMMAND_BAR_ENTRY_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_COMMAND_BAR_ENTRY_H_

#include <optional>
#include <string>
#include <string_view>
#include "base/functional/callback.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "ui/base/models/image_model.h"
#include "url/gurl.h"

namespace krypton_ui {
// Owned by one suggestion snapshot. Execution retains its original weak target.
struct CommandBarEntry {
  std::string title;
  std::string description;
  base::OnceClosure execute;
  uint32_t kind = KR_SUGGESTION_COMMAND;
  int rank = 0;
  // Leased into the suggestion snapshot only while it is shown.
  ui::ImageModel icon;
  // An open tab's page, so an address edit can show it once, as a switch.
  GURL url;
};
// Query-time fuzzy subsequence matching; never used by page rendering.
bool MatchesCommand(std::string_view query,
                    std::string_view title,
                    std::string_view aliases = {});
std::optional<std::string> ExpandBangSearch(std::string_view text);
}  // namespace krypton_ui
#endif
