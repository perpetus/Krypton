// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MENU_GLYPHS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MENU_GLYPHS_H_

#include <string_view>

namespace krypton_ui {

// Segoe Fluent Icons glyphs, as UTF-8, for the rows KryptonUI's preview menus
// show. Empty when a command has none; the row then keeps the model's icon.
// Browser command ids, layout commands and tab menu commands are separate id
// spaces, so each menu picks the table for its own source model.
std::string_view BrowserCommandGlyph(int command_id);
std::string_view TabCommandGlyph(int command_id);
// The tab menu's mute rows show the unmute glyph while the tab is muted.
std::string_view MutedTabCommandGlyph(int command_id);

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_MENU_GLYPHS_H_
