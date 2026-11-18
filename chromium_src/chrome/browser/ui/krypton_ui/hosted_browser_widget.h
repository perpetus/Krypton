// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_HOSTED_BROWSER_WIDGET_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_HOSTED_BROWSER_WIDGET_H_

class BrowserNativeWidget;
class BrowserView;
class BrowserWidget;

namespace views {
class Widget;
}

namespace krypton_ui {
// A Space of another persistent profile is hosted by a real Browser of that
// profile. Its BrowserWidget is a child widget of the window's frame, so both
// profiles share one HWND and compositor while keeping their own Browser,
// TabStripModel and services. Returns nullptr for an ordinary browser window.
// Called by BrowserNativeWidgetFactory.
BrowserNativeWidget* CreateHostedBrowserNativeWidget(BrowserWidget* widget,
                                                     BrowserView* view);

// Shows or hides a hosted widget and reports its derived activation, which
// follows the frame's activation while presented. No-op for other widgets.
void SetHostedWidgetPresented(views::Widget* widget, bool presented);
void SetHostedWidgetActive(views::Widget* widget, bool active);
// Whether Chromium has asked this hosted widget to show at least once.
[[nodiscard]] bool HostedWidgetWasShown(views::Widget* widget);
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_HOSTED_BROWSER_WIDGET_H_
