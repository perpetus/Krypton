// Copyright 2026 Krypton Authors
// Use of this source code is governed by the license in the LICENSE file.
#ifndef CHROME_BROWSER_WIN_KRYPTON_UPDATE_SERVICE_H_
#define CHROME_BROWSER_WIN_KRYPTON_UPDATE_SERVICE_H_

namespace krypton {
// Verifies the installed helper off the UI thread, then starts one background
// update check. The helper owns downloads and waits for browser exit to apply.
void MaybeStartSignedUpdater();
// Returns true when the verified helper owns a pending update and will reopen
// the browser after applying it. The browser should then exit without its own
// immediate relaunch, so executable locks can actually be released.
bool PrepareSignedUpdateRelaunch();
}  // namespace krypton

#endif  // CHROME_BROWSER_WIN_KRYPTON_UPDATE_SERVICE_H_
