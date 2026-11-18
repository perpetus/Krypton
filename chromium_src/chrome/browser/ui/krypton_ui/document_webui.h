// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_DOCUMENT_WEBUI_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_DOCUMENT_WEBUI_H_
namespace content {
class WebContents;
}
namespace krypton_ui {
// Registration stores factories only. No profile service, disk access or editor
// code is loaded until a document URL is opened.
void RegisterDocumentWebUIConfigs();
void SetDocumentPageTheme(content::WebContents* contents, bool dark);
}  // namespace krypton_ui
#endif
