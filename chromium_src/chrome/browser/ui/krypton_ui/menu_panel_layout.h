// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MENU_PANEL_LAYOUT_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MENU_PANEL_LAYOUT_H_

#include <cstdint>

namespace krypton_ui {
struct MenuPanelRow {
  int source_command;
  uint32_t type;
};
}  // namespace krypton_ui

#endif
