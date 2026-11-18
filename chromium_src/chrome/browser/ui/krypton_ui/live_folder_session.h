// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_LIVE_FOLDER_SESSION_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_LIVE_FOLDER_SESSION_H_
#include <cstdint>
#include <map>
#include <string>
namespace content {
class WebContents;
}
namespace krypton_ui {
inline constexpr char kLiveFolderItemSessionKey[] = "krypton_ui.live_item";
struct LiveFolderTabState {
  uint64_t folder = 0;
  std::string item;
  bool accessed = false;
};
LiveFolderTabState LiveFolderStateForTab(const content::WebContents *contents);
std::string EncodeLiveFolderTabState(const LiveFolderTabState &state);
void SetLiveFolderTabState(content::WebContents *contents,
                           LiveFolderTabState state);
void AddLiveFolderSessionData(const content::WebContents *contents,
                              std::map<std::string, std::string> &extra_data);
void RestoreLiveFolderSessionData(
    content::WebContents *contents,
    const std::map<std::string, std::string> &extra_data);
} // namespace krypton_ui
#endif
