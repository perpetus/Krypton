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
inline constexpr char kPinnedHomeSessionKey[] = "krypton_ui.pinned_home";
std::string PinnedHomeForTab(const content::WebContents* contents);
void SetPinnedHomeForTab(content::WebContents* contents, std::string_view url);
inline constexpr char kFolderSessionKey[] = "krypton_ui.folder";
// Profile directory base name of the catalog that owns the tab's Space and
// folder ids, written only when it differs from the tab's own profile: a tab
// hosted for another profile's window. Space ids are unique per catalog, not
// globally. Essential ids always belong to the tab's own profile.
inline constexpr char kWorkspaceCatalogSessionKey[] =
    "krypton_ui.workspace_catalog";
// Window extra data: a durable window identity, and for a profile host the
// identity of the window whose frame it shares.
inline constexpr char kWindowIdSessionKey[] = "krypton_ui.window";
inline constexpr char kSpaceHostSessionKey[] = "krypton_ui.space_host";
struct WorkspaceTabSessionState {
  uint64_t workspace = 0;
  uint64_t essential = 0;
  uint64_t folder = 0;
  std::string catalog;  // Empty: the tab's own profile.
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
// Catalog owner of the tab's Space/folder ids; empty for the tab's own profile.
std::string WorkspaceCatalogForTab(const content::WebContents* contents);
void SetWorkspaceCatalogForTab(content::WebContents* contents,
                               std::string_view catalog);
// Krypton window data re-emitted with the window's session commands. Keyed by
// the window's SessionID value; UI thread only.
void SetWindowSessionData(int32_t window_id,
                          std::map<std::string, std::string> data);
void ClearWindowSessionData(int32_t window_id);
void AddWindowSessionData(int32_t window_id,
                          std::map<std::string, std::string>& extra_data);
// Session restore's window extra data, visible only while it synchronously
// creates that window.
class ScopedRestoredWindow {
 public:
  explicit ScopedRestoredWindow(
      const std::map<std::string, std::string>& extra_data);
  ScopedRestoredWindow(const ScopedRestoredWindow&) = delete;
  ScopedRestoredWindow& operator=(const ScopedRestoredWindow&) = delete;
  ~ScopedRestoredWindow();

 private:
  const std::map<std::string, std::string>* previous_;
};
[[nodiscard]] const std::map<std::string, std::string>* RestoredWindowData();
void RestoreWorkspaceForTab(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data);
}  // namespace krypton_ui
#endif
