// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SITE_INFO_MENU_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SITE_INFO_MENU_H_

#include <memory>
#include <string_view>

class BrowserWindowInterface;
namespace tabs {
class TabInterface;
}
namespace content {
class WebContents;
}
namespace ui {
class MenuModel;
}

namespace krypton_ui {
std::unique_ptr<ui::MenuModel> CreateSiteInfoMenu(
    BrowserWindowInterface& browser,
    tabs::TabInterface& tab);
std::string_view SiteInfoGlyph(int command);
bool SiteInfoKeepsOpen(int command);
bool SiteInfoIsValid(const ui::MenuModel& menu);
// Handles Chromium's page-info command for a Krypton window. False means the
// contents belong to a different native shell and should use its own UI.
bool RequestSiteInfo(content::WebContents* contents);
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_SITE_INFO_MENU_H_
