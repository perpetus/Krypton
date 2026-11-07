// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_CLOSE_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_CLOSE_H_

class TabStripModel;
namespace content {
class WebContents;
}
namespace tabs {
class TabInterface;
}

namespace krypton_ui {
// Other browser types retain Chromium's unfiltered selection policy.
bool IsWorkspaceCloseCandidate(const TabStripModel& model,
                               const tabs::TabInterface* candidate);
bool NeedsWorkspaceCloseTarget(content::WebContents* closing);
void PrepareWorkspaceCloseTarget(content::WebContents* closing);
}  // namespace krypton_ui

#endif
