// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/command_bar_entry.h"
#include "chrome/browser/ui/krypton_ui/page_capture.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace krypton_ui {
std::optional<std::string> ExpandBangSearch(std::string_view text) {
  if (!text.starts_with("!"))
    return std::nullopt;
  const auto split = text.find_first_of(" \t");
  if (split == std::string_view::npos)
    return std::nullopt;
  const std::string bang = base::ToLowerASCII(text.substr(0, split));
  std::string_view query = text.substr(split + 1);
  query = base::TrimWhitespaceASCII(query, base::TRIM_ALL);
  if (query.empty())
    return std::nullopt;
  const auto escaped = base::EscapeQueryParamValue(query, true);
  if (bang == "!g")
    return "https://www.google.com/search?q=" + escaped;
  if (bang == "!w")
    return "https://en.wikipedia.org/w/index.php?search=" + escaped;
  if (bang == "!yt")
    return "https://www.youtube.com/results?search_query=" + escaped;
  if (bang == "!gh")
    return "https://github.com/search?q=" + escaped;
  return "https://duckduckgo.com/?q=" + base::EscapeQueryParamValue(text, true);
}
bool MatchesCommand(std::string_view query,
                    std::string_view title,
                    std::string_view aliases) {
  // Keep the normalized query alive: SplitStringPiece borrows its input.
  const std::string normalized = base::ToLowerASCII(query);
  const auto terms = base::SplitStringPiece(
      normalized, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  const std::string searchable =
      base::ToLowerASCII(std::string(title) + " " + std::string(aliases));
  for (auto term : terms) {
    if (searchable.find(term) != std::string::npos)
      continue;
    size_t at = 0;
    for (const char c : searchable)
      if (at < term.size() && c == term[at])
        ++at;
    if (at != term.size())
      return false;
  }
  return true;
}
std::vector<CommandBarEntry> BrowserAdapter::CommandBarEntries(
    std::string_view query) {
  if (query.size() > 256 || closed_ || !browser_initialized_ || !presented_)
    return {};
  const auto matches = [query](std::string_view title,
                               std::string_view aliases) {
    return MatchesCommand(query, title, aliases);
  };
  struct Command {
    int id;
    const char* title;
    const char* aliases;
    bool tab;
  };
  static constexpr Command commands[] = {
      {IDC_NEW_TAB, "New tab", "open create", false},
      {IDC_NEW_WINDOW, "New window", "open create", false},
      {IDC_NEW_INCOGNITO_WINDOW, "New private window", "incognito", false},
      {IDC_CLOSE_TAB, "Close tab", "remove", true},
      {IDC_RESTORE_TAB, "Reopen closed tab", "restore undo", false},
      {IDC_DUPLICATE_TAB, "Duplicate tab", "copy", true},
      {IDC_SELECT_NEXT_TAB, "Next tab", "switch cycle", true},
      {IDC_SELECT_PREVIOUS_TAB, "Previous tab", "switch cycle", true},
      {IDC_RELOAD, "Reload page", "refresh", true},
      {IDC_RELOAD_BYPASSING_CACHE, "Hard reload page", "refresh bypass cache",
       true},
      {IDC_RELOAD_CLEARING_CACHE, "Empty cache and reload", "hard refresh",
       true},
      {IDC_STOP, "Stop loading", "cancel", true},
      {IDC_BACK, "Go back", "history previous", true},
      {IDC_FORWARD, "Go forward", "history next", true},
      {IDC_FIND, "Find in page", "search text", true},
      {IDC_ZOOM_PLUS, "Zoom in", "larger", true},
      {IDC_ZOOM_MINUS, "Zoom out", "smaller", true},
      {IDC_ZOOM_NORMAL, "Reset zoom", "actual size 100", true},
      {IDC_FULLSCREEN, "Toggle full screen", "fullscreen", false},
      {IDC_BOOKMARK_THIS_TAB, "Bookmark page", "save favorite", true},
      {IDC_SHOW_HISTORY, "Open history", "visited library", false},
      {IDC_SHOW_BOOKMARK_MANAGER, "Open bookmarks", "favorites library", false},
      {IDC_SHOW_DOWNLOADS, "Open downloads", "files library", false},
      {IDC_OPTIONS, "Open settings", "preferences options", false},
      {IDC_MANAGE_EXTENSIONS, "Manage extensions", "addons plugins", false},
      {IDC_SHOW_PASSWORD_MANAGER, "Open password manager", "passwords logins",
       false},
      {IDC_DEV_TOOLS, "Open developer tools", "devtools inspect", true},
      {IDC_DEV_TOOLS_CONSOLE, "Open JavaScript console", "devtools developer",
       true},
      {IDC_DEV_TOOLS_INSPECT, "Inspect elements", "devtools developer picker",
       true},
      {IDC_VIEW_SOURCE, "View page source", "html developer", true},
      {IDC_TASK_MANAGER_MAIN_MENU, "Open task manager",
       "processes memory performance", false},
      {IDC_PRINT, "Print page", "pdf", true},
      {IDC_SAVE_PAGE, "Save page", "download html offline", true},
      {IDC_CLEAR_BROWSING_DATA, "Clear browsing data",
       "delete history cookies cache", false},
  };
  std::vector<CommandBarEntry> entries;
  for (const auto& command : commands) {
    if (matches(command.title, command.aliases)) {
      auto execute = CaptureCommand(command.id, command.tab);
      if (execute)
        entries.push_back(
            {command.title, "Browser command", std::move(execute)});
    }
  }
  const auto owner = weak_factory_.GetWeakPtr();
  auto* active = browser_->GetActiveTabInterface();
  if (active) {
    const auto tab = active->GetWeakPtr();
    if (CanTakeScreenshot()) {
      // The selection overlay handles both regions and elements.
      for (const auto& [mode, title] :
           {std::pair{PageCaptureMode::kArea, "Screenshot: select region"},
            std::pair{PageCaptureMode::kFullPage, "Screenshot: full page"}}) {
        if (matches(title, "capture screen image png area element"))
          entries.push_back({title, "Copy or save an image",
                             base::BindOnce(
                                 [](base::WeakPtr<BrowserAdapter> owner,
                                    base::WeakPtr<tabs::TabInterface> tab,
                                    PageCaptureMode mode) {
                                   if (owner && tab)
                                     owner->TakeScreenshot(tab.get(), mode);
                                 },
                                 owner, tab, mode)});
        if (CanCreateDocument() &&
            matches(title, "add capture to easel screenshot canvas"))
          entries.push_back(
              {std::string("Add to Easel: ") +
                   (mode == PageCaptureMode::kFullPage ? "full page"
                                                       : "select region"),
               "Capture into a new or existing easel",
               base::BindOnce(
                   [](base::WeakPtr<BrowserAdapter> owner,
                      base::WeakPtr<tabs::TabInterface> tab,
                      PageCaptureMode mode) {
                     if (owner && tab && owner->CanCreateDocument())
                       owner->TakeScreenshot(tab.get(), mode, true);
                   },
                   owner, tab, mode)});
      }
    }
    if (matches("Copy page link", "url address clipboard"))
      entries.push_back({"Copy page link", "Copy to clipboard",
                         base::BindOnce(
                             [](base::WeakPtr<BrowserAdapter> owner,
                                base::WeakPtr<tabs::TabInterface> tab) {
                               if (owner && tab &&
                                   owner->browser_->GetActiveTabInterface() ==
                                       tab.get())
                                 owner->CopyCurrentLink();
                             },
                             owner, tab)});
    const auto add_tab_command = [&](TabStripModel::ContextMenuCommand command,
                                     const char* title, const char* aliases) {
      if (matches(title, aliases))
        entries.push_back(
            {title, "Current tab",
             base::BindOnce(
                 [](base::WeakPtr<BrowserAdapter> owner,
                    base::WeakPtr<tabs::TabInterface> tab,
                    TabStripModel::ContextMenuCommand command) {
                   if (!owner || !tab ||
                       owner->browser_->GetActiveTabInterface() != tab.get())
                     return;
                   auto* model = owner->browser_->GetTabStripModel();
                   const int index = model->GetIndexOfTab(tab.get());
                   if (index >= 0 &&
                       model->IsContextMenuCommandEnabled(index, command))
                     model->ExecuteContextMenuCommand(index, command);
                 },
                 owner, tab, command)});
    };
    add_tab_command(TabStripModel::CommandTogglePinned, "Pin or unpin tab",
                    "keep pinned");
    add_tab_command(TabStripModel::CommandToggleSiteMuted,
                    "Mute or unmute site", "audio sound");
  }
  for (const auto& space : workspaces_.spaces) {
    const std::string title = "Switch to Space: " + space.name;
    if (space.id != active_space_ && matches(title, "workspace profile"))
      entries.push_back(
          {title, "Switch Space",
           base::BindOnce(&BrowserAdapter::SwitchWorkspace, owner, space.id)});
  }
  AppendExtraCommands(query, entries);
  if (!query.empty()) {
    const auto normalized = base::ToLowerASCII(query);
    for (auto& entry : entries) {
      const auto title = base::ToLowerASCII(entry.title);
      const int match = title == normalized                           ? 0
                        : title.find(normalized) != std::string::npos ? 1
                                                                      : 2;
      entry.rank = match * 2 + (entry.kind == KR_SUGGESTION_OPEN_TAB ? 0 : 1);
    }
    std::stable_sort(
        entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.rank < b.rank; });
  }
  if (entries.size() > 64)
    entries.resize(64);
  return entries;
}
}  // namespace krypton_ui
