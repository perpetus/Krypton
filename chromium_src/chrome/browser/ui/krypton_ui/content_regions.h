// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_CONTENT_REGIONS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_CONTENT_REGIONS_H_

#include "ui/gfx/geometry/rect.h"

namespace krypton_ui {

struct ContentRegions {
  gfx::Rect side_panel;
  gfx::Rect bookmarks;
  gfx::Rect infobars;
  gfx::Rect page;
};

// Partitions only the module-provided content area using measured Chromium
// service sizes. No browser-chrome dimensions or styling values live here.
[[nodiscard]] ContentRegions PartitionContent(gfx::Rect area,
                                              int side_panel_width,
                                              bool side_panel_right,
                                              int bookmark_height,
                                              int infobar_height);

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_CONTENT_REGIONS_H_
