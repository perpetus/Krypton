// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_COMMANDS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_COMMANDS_H_
#include <cstdint>
namespace content {
class WebContents;
}
class BrowserWindowInterface;
namespace tabs {
class TabInterface;
}
namespace krypton_ui {
inline constexpr int kOpenLinkInGlance = 0x6d200000;
// UI/navigation commands can join Krypton's bounded groups without importing
// the controller or ABI. Native model APIs retain registered native split IDs.
[[nodiscard]] bool RedirectNativeSplit(tabs::TabInterface* pivot,
                                       tabs::TabInterface* other,
                                       bool stacked);
// Used by link menus before creating a companion tab.
[[nodiscard]] bool MultiPageSplitFull(tabs::TabInterface* tab);
[[nodiscard]] bool ShowMultiPageSplitChooser(BrowserWindowInterface* browser);
// Queue after link split creation so the authoritative group/revision exists.
void SetMultiPageSplitLayout(content::WebContents* source, uint32_t layout);
}  // namespace krypton_ui
#endif
