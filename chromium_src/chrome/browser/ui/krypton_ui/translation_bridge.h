// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_TRANSLATION_BRIDGE_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_TRANSLATION_BRIDGE_H_
namespace content {
class WebContents;
}
namespace krypton_ui {
// Push translation state to an existing shell; never opens a popup or steals
// focus. Returns whether a native shell owns this document's browser.
bool UpdateTranslationUI(content::WebContents* contents,
                         bool failed = false,
                         bool open_menu = false);
}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_TRANSLATION_BRIDGE_H_
