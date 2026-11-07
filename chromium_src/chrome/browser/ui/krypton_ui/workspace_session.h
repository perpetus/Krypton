// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_SESSION_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_SESSION_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace content {
class WebContents;
}
namespace krypton_ui {
inline constexpr char kWorkspaceSessionKey[] = "krypton_ui.workspace";
inline constexpr char kEssentialSessionKey[] = "krypton_ui.essential";
inline constexpr char kTabIconSessionKey[] = "krypton_ui.tab_icon";
inline constexpr char kFolderSessionKey[] = "krypton_ui.folder";
struct WorkspaceTabSessionState {
  uint64_t workspace = 0;
  uint64_t essential = 0;
  uint64_t folder = 0;
  bool operator==(const WorkspaceTabSessionState&) const = default;
};
WorkspaceTabSessionState DecodeWorkspaceSessionData(
    const std::map<std::string, std::string>& extra_data);
void EncodeWorkspaceSessionData(WorkspaceTabSessionState state,
                                std::map<std::string, std::string>& extra_data);
void AddWorkspaceSessionData(const content::WebContents* contents,
                             std::map<std::string, std::string>& extra_data);
std::string TabIconForTab(const content::WebContents* contents);
void SetTabIconForTab(content::WebContents* contents, std::string_view glyph);
uint64_t EssentialForTab(const content::WebContents* contents);
void SetEssentialForTab(content::WebContents* contents, uint64_t id);
uint64_t FolderForTab(const content::WebContents* contents);
void SetFolderForTab(content::WebContents* contents, uint64_t id);
[[nodiscard]] uint64_t ParseWorkspaceId(std::string_view value);
[[nodiscard]] uint64_t WorkspaceForTab(const content::WebContents* contents);
void SetWorkspaceForTab(content::WebContents* contents, uint64_t id);
void RestoreWorkspaceForTab(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data);
}  // namespace krypton_ui
#endif
