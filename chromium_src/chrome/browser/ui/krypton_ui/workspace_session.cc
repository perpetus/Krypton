// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/krypton_ui/live_folder_session.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"

#include <algorithm>

#include "base/files/file_path.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/ui/krypton_ui/essential_prefs.h"
#include "components/sessions/core/session_types.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"

namespace krypton_ui {
namespace {
class WorkspaceTabData : public content::WebContentsUserData<WorkspaceTabData> {
 public:
  uint64_t id = 0;
  uint64_t essential = 0;
  uint64_t folder = 0;
  std::string icon;
  std::string pinned_home;
  std::string catalog;

 private:
  friend class content::WebContentsUserData<WorkspaceTabData>;
  explicit WorkspaceTabData(content::WebContents* contents)
      : content::WebContentsUserData<WorkspaceTabData>(*contents) {}
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(WorkspaceTabData);
}  // namespace
std::string PinnedHomeForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->pinned_home : std::string();
}
void SetPinnedHomeForTab(content::WebContents* contents, std::string_view url) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->pinned_home = url;
}
std::string TabIconForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->icon : std::string();
}
void SetTabIconForTab(content::WebContents* contents, std::string_view glyph) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->icon = glyph;
}
uint64_t ParseWorkspaceId(std::string_view value) {
  uint64_t id = 0;
  return base::StringToUint64(value, &id) ? id : 0;
}
uint64_t WorkspaceForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->id : 0;
}
void SetWorkspaceForTab(content::WebContents* contents, uint64_t id) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->id = id;
}
std::string WorkspaceCatalogForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->catalog : std::string();
}
void SetWorkspaceCatalogForTab(content::WebContents* contents,
                               std::string_view catalog) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->catalog = catalog;
}
uint64_t EssentialForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->essential : 0;
}
uint64_t FolderForTab(const content::WebContents* contents) {
  const auto* data = WorkspaceTabData::FromWebContents(contents);
  return data ? data->folder : 0;
}
void SetFolderForTab(content::WebContents* contents, uint64_t id) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->folder = id;
}
void SetEssentialForTab(content::WebContents* contents, uint64_t id) {
  WorkspaceTabData::CreateForWebContents(contents);
  WorkspaceTabData::FromWebContents(contents)->essential = id;
}
WorkspaceTabSessionState DecodeWorkspaceSessionData(
    const std::map<std::string, std::string>& extra_data) {
  const auto read = [&](const char* key) {
    const auto found = extra_data.find(key);
    return found == extra_data.end() ? uint64_t{0}
                                     : ParseWorkspaceId(found->second);
  };
  WorkspaceTabSessionState state{read(kWorkspaceSessionKey),
                                 read(kEssentialSessionKey),
                                 read(kFolderSessionKey)};
  if (const auto found = extra_data.find(kWorkspaceCatalogSessionKey);
      found != extra_data.end()) {
    // Keep oversized values unusable rather than truncating them into a match.
    state.catalog = found->second.size() <= 255 ? found->second : "/";
  }
  return state;
}
void EncodeWorkspaceSessionData(
    WorkspaceTabSessionState state,
    std::map<std::string, std::string>& extra_data) {
  if (state.workspace)
    extra_data[kWorkspaceSessionKey] = base::NumberToString(state.workspace);
  else
    extra_data.erase(kWorkspaceSessionKey);
  extra_data[kEssentialSessionKey] = base::NumberToString(state.essential);
  if (state.folder)
    extra_data[kFolderSessionKey] = base::NumberToString(state.folder);
  else
    extra_data.erase(kFolderSessionKey);
  if (state.catalog.empty())
    extra_data.erase(kWorkspaceCatalogSessionKey);
  else
    extra_data[kWorkspaceCatalogSessionKey] = state.catalog;
}
void AddWorkspaceSessionData(const content::WebContents* contents,
                             std::map<std::string, std::string>& extra_data) {
  AddMultiPageSessionData(contents, extra_data);
  const auto home = PinnedHomeForTab(contents);
  if (home.empty())
    extra_data.erase(kPinnedHomeSessionKey);
  else
    extra_data[kPinnedHomeSessionKey] = home;
  AddLiveFolderSessionData(contents, extra_data);
  const auto icon = TabIconForTab(contents);
  if (icon.empty())
    extra_data.erase(kTabIconSessionKey);
  else
    extra_data[kTabIconSessionKey] = icon;
  EncodeWorkspaceSessionData(
      {WorkspaceForTab(contents), EssentialForTab(contents),
       FolderForTab(contents), WorkspaceCatalogForTab(contents)},
      extra_data);
}
void RestoreWorkspaceForTab(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data) {
  RestoreMultiPageSessionData(contents, extra_data);
  if (const auto found = extra_data.find(kPinnedHomeSessionKey);
      found != extra_data.end() && found->second.size() <= 65536)
    SetPinnedHomeForTab(contents, found->second);
  if (const auto found = extra_data.find(kTabIconSessionKey);
      found != extra_data.end() && found->second.size() <= 128)
    SetTabIconForTab(contents, found->second);
  auto state = DecodeWorkspaceSessionData(extra_data);
  if (state.catalog ==
      contents->GetBrowserContext()->GetPath().BaseName().AsUTF8Unsafe()) {
    state.catalog.clear();
  }
  // Keep foreign catalog ids with their owner. The adapter that receives the
  // tab accepts them only for that catalog; a same-numbered local Space never
  // inherits them. Live Folders belong to the tab's own catalog only.
  if (state.catalog.empty())
    RestoreLiveFolderSessionData(contents, extra_data);
  SetWorkspaceForTab(contents, state.workspace);
  SetEssentialForTab(contents, state.essential);
  SetFolderForTab(contents, state.folder);
  SetWorkspaceCatalogForTab(contents, state.catalog);
}

void PruneRecoveredEssentials(
    const PrefService& prefs,
    std::vector<std::unique_ptr<sessions::SessionWindow>>& windows) {
  const auto catalog = ReadEssentials(prefs);
  if (catalog.items.empty()) {
    return;
  }
  // Essential ids always belong to the tab's own profile, the one restoring.
  const auto saved = [&](const std::unique_ptr<sessions::SessionTab>& tab) {
    const auto id = DecodeWorkspaceSessionData(tab->extra_data).essential;
    return id && std::ranges::contains(catalog.items, id, &Essential::id);
  };
  for (auto& window : windows) {
    auto& tabs = window->tabs;
    if (window->type != sessions::SessionWindow::TYPE_NORMAL || tabs.empty() ||
        std::ranges::all_of(tabs, saved)) {
      continue;
    }
    // The selected tab keeps its place; a removed one hands the selection to
    // the next remaining tab, or the last one.
    const auto selected = static_cast<size_t>(std::clamp<int>(
        window->selected_tab_index, 0, static_cast<int>(tabs.size()) - 1));
    const auto before =
        std::count_if(tabs.begin(), tabs.begin() + selected,
                      [&](const auto& tab) { return !saved(tab); });
    std::erase_if(tabs, saved);
    window->selected_tab_index = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(before), tabs.size() - 1));
  }
}

namespace {
std::map<int32_t, std::map<std::string, std::string>>& WindowData() {
  static base::NoDestructor<
      std::map<int32_t, std::map<std::string, std::string>>>
      data;
  return *data;
}
const std::map<std::string, std::string>* g_restored_window = nullptr;
}  // namespace

void SetWindowSessionData(int32_t window_id,
                          std::map<std::string, std::string> data) {
  WindowData()[window_id] = std::move(data);
}
void ClearWindowSessionData(int32_t window_id) {
  WindowData().erase(window_id);
}
void AddWindowSessionData(int32_t window_id,
                          std::map<std::string, std::string>& extra_data) {
  if (const auto found = WindowData().find(window_id);
      found != WindowData().end()) {
    for (const auto& [key, value] : found->second)
      extra_data[key] = value;
  }
}
ScopedRestoredWindow::ScopedRestoredWindow(
    const std::map<std::string, std::string>& extra_data)
    : previous_(g_restored_window) {
  g_restored_window = &extra_data;
}
ScopedRestoredWindow::~ScopedRestoredWindow() {
  g_restored_window = previous_;
}
const std::map<std::string, std::string>* RestoredWindowData() {
  return g_restored_window;
}
}  // namespace krypton_ui
