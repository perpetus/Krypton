// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/content_regions.h"

#include <algorithm>

namespace krypton_ui {

ContentRegions PartitionContent(gfx::Rect area,
                                int side_panel_width,
                                bool side_panel_right,
                                int bookmark_height,
                                int infobar_height) {
  const int panel_width = std::clamp(side_panel_width, 0, area.width());
  ContentRegions result;
  result.side_panel =
      gfx::Rect(area.origin(), gfx::Size(panel_width, area.height()));
  if (side_panel_right) {
    result.side_panel.set_x(area.right() - panel_width);
    area.set_width(area.width() - panel_width);
  } else {
    area.Inset(gfx::Insets::TLBR(0, panel_width, 0, 0));
  }
  const int bookmarks = std::clamp(bookmark_height, 0, area.height());
  result.bookmarks =
      gfx::Rect(area.origin(), gfx::Size(area.width(), bookmarks));
  area.Inset(gfx::Insets::TLBR(bookmarks, 0, 0, 0));
  const int infobars = std::clamp(infobar_height, 0, area.height());
  result.infobars = gfx::Rect(area.origin(), gfx::Size(area.width(), infobars));
  area.Inset(gfx::Insets::TLBR(infobars, 0, 0, 0));
  result.page = area;
  return result;
}

}  // namespace krypton_ui
