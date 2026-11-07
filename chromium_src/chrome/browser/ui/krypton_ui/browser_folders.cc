// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include "base/auto_reset.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace krypton_ui {
bool BrowserAdapter::CanFolders() const {
  return !browser_->GetProfile()->IsOffTheRecord();
}
void BrowserAdapter::PublishFolders() {
  const auto* api = session_.api();
  if (!CanFolders() || !api || !KR_HAS_SLOT(api, KrShellUI, remove_folder) ||
      !api->upsert_folder || !api->remove_folder)
    return;
  bool reset = false;
  for (const auto& previous : published_folders_) {
    const auto* current = folders_.Find(previous.id);
    if (!current || current->space != previous.space) {
      std::ignore = api->remove_folder(api->self, previous.id);
      reset = true;
    }
  }
  // Parent-first publication also reconstructs a subtree moved to a workspace.
  for (unsigned depth = 1; depth <= 5; ++depth) {
    for (const auto& folder : folders_.items) {
      unsigned level = 1;
      const auto* parent = folders_.Find(folder.parent);
      while (parent && level <= 5) {
        ++level;
        parent = folders_.Find(parent->parent);
      }
      if (level != depth)
        continue;
      const auto old =
          std::ranges::find(published_folders_, folder.id, &TabFolder::id);
      if (!reset && old != published_folders_.end() && *old == folder)
        continue;
      const KrFolder value{sizeof(value),
                           folder.id,
                           folder.space,
                           folder.parent,
                           {folder.name.data(), folder.name.size()},
                           {folder.glyph.data(), folder.glyph.size()},
                           folder.order,
                           folder.collapsed};
      if (api->upsert_folder(api->self, &value) != KR_OK)
        return;
    }
  }
  published_folders_ = folders_.items;
}
void BrowserAdapter::OnFoldersChanged() {
  if (!CanFolders() || closed_)
    return;
  folders_ = ReadFolders(*browser_->GetProfile()->GetPrefs());
  PublishFolders();
  std::vector<KrTabId> tabs;
  for (auto* tab : *browser_->GetTabStripModel())
    tabs.push_back(Identity(*tab));
  for (const auto tab_id : tabs) {
    auto* tab = FindTab(tab_id);
    if (!tab)
      continue;
    const auto id = FolderForTab(tab->GetContents());
    const auto* folder = folders_.Find(id);
    if (id && !folder)
      AssignFolder(*tab, 0);
    else if (folder && WorkspaceForTab(tab->GetContents()) != folder->space) {
      AssignWorkspace(*tab, folder->space);
      AssignFolder(*tab, folder->id);
    } else
      PublishTab(*tab);
  }
}
void BrowserAdapter::AssignFolder(tabs::TabInterface& tab, KrFolderId id) {
  if (!editing_folders_) {
    if (const auto split = SplitSessionForTab(tab.GetContents())) {
      std::vector<KrTabId> members;
      for (auto* member : *browser_->GetTabStripModel())
        if (const auto state = SplitSessionForTab(member->GetContents());
            state && state->group == split->group)
          members.push_back(Identity(*member));
      const base::AutoReset<bool> editing(&editing_folders_, true);
      for (const auto member : members)
        if (auto* current = FindTab(member))
          AssignFolder(*current, id);
      return;
    }
  }
  const auto* folder = folders_.Find(id);
  if (id && !folder)
    return;
  if (folder) {
    if (EssentialForTab(tab.GetContents()))
      SetEssential(tab, false);
    if (WorkspaceForTab(tab.GetContents()) != folder->space)
      AssignWorkspace(tab, folder->space);
  }
  SetFolderForTab(tab.GetContents(), id);
  if (auto* service =
          SessionServiceFactory::GetForProfile(browser_->GetProfile())) {
    const auto session_id =
        sessions::SessionTabHelper::IdForTab(tab.GetContents());
    if (session_id.is_valid())
      service->AddTabExtraData(browser_->GetSessionID(), session_id,
                               kFolderSessionKey, base::NumberToString(id));
  }
  if (folder && !tab.IsPinned()) {
    auto* model = browser_->GetTabStripModel();
    model->SetTabPinned(model->GetIndexOfTab(&tab), true);
  }
  PublishTab(tab);
}
KrResult BrowserAdapter::FolderCommand(const KrFolderCommand& command) {
  const auto* api = session_.api();
  if (!CanFolders() || closed_ || !api ||
      !KR_HAS_SLOT(api, KrShellUI, remove_folder) || !api->upsert_folder ||
      !api->remove_folder)
    return KR_ERR_UNSUPPORTED;
  if (!KR_HAS_FIELD(&command, KrFolderCommand, collapsed) ||
      command.kind > KR_FOLDER_NEW_TAB ||
      (!command.name.data && command.name.size) ||
      (!command.glyph.data && command.glyph.size))
    return KR_ERR_INVALID_ARGUMENT;
  if (command.kind == KR_FOLDER_MOVE_TAB) {
    auto* tab = FindTab(command.tab);
    if (!tab || (command.folder && !folders_.Find(command.folder)))
      return KR_ERR_INVALID_ARGUMENT;
    AssignFolder(*tab, command.folder);
    PublishSelection();
    return KR_OK;
  }
  auto catalog = folders_;
  auto found = std::ranges::find(catalog.items, command.folder, &TabFolder::id);
  if (command.kind != KR_FOLDER_CREATE && found == catalog.items.end())
    return KR_ERR_INVALID_ARGUMENT;
  if (command.kind == KR_FOLDER_NEW_TAB) {
    const base::AutoReset<KrSpaceId> space(&new_tab_space_, found->space);
    const base::AutoReset<KrFolderId> folder(&new_folder_, found->id);
    chrome::NewTab(&*browser_, NewTabTypes::kNewTabCommand);
    if (auto* tab = browser_->GetActiveTabInterface())
      AssignFolder(*tab, found->id);
    return KR_OK;
  }
  KrFolderId created = 0;
  std::vector<KrTabId> close;
  switch (command.kind) {
    case KR_FOLDER_CREATE: {
      if (catalog.next_id == std::numeric_limits<KrFolderId>::max())
        return KR_ERR_INVALID_ARGUMENT;
      const auto* parent = catalog.Find(command.parent);
      if (command.parent && !parent)
        return KR_ERR_INVALID_ARGUMENT;
      TabFolder value;
      value.id = created = catalog.next_id++;
      value.parent = command.parent;
      value.space = parent          ? parent->space
                    : command.space ? command.space
                                    : active_space_;
      value.name = command.name.size
                       ? std::string(command.name.data, command.name.size)
                       : "Folder";
      value.glyph = command.glyph.size
                        ? std::string(command.glyph.data, command.glyph.size)
                        : "";
      value.order = catalog.items.size();
      catalog.items.push_back(std::move(value));
      break;
    }
    case KR_FOLDER_RENAME:
      found->name = command.name.size
                        ? std::string(command.name.data, command.name.size)
                        : "";
      base::TrimWhitespaceASCII(found->name, base::TRIM_ALL, &found->name);
      found->glyph = command.glyph.size
                         ? std::string(command.glyph.data, command.glyph.size)
                         : "";
      break;
    case KR_FOLDER_COLLAPSE:
      found->collapsed = command.collapsed;
      break;
    case KR_FOLDER_MOVE: {
      const auto target_space = command.parent && catalog.Find(command.parent)
                                    ? catalog.Find(command.parent)->space
                                : command.space ? command.space
                                                : found->space;
      for (auto& folder : catalog.items)
        if (folders_.IsDescendant(folder.id, command.folder))
          folder.space = target_space;
      found->parent = command.parent;
      std::vector<TabFolder*> siblings;
      for (auto& folder : catalog.items)
        if (folder.id != command.folder && folder.parent == found->parent &&
            folder.space == found->space)
          siblings.push_back(&folder);
      std::ranges::sort(siblings, [](const TabFolder* a, const TabFolder* b) {
        return a->order != b->order ? a->order < b->order : a->id < b->id;
      });
      siblings.insert(
          siblings.begin() + std::min(command.index, siblings.size()), &*found);
      for (size_t i = 0; i < siblings.size(); ++i)
        siblings[i]->order = i;
      break;
    }
    case KR_FOLDER_UNPACK:
    case KR_FOLDER_DELETE:
      for (auto* tab : *browser_->GetTabStripModel())
        if (catalog.IsDescendant(FolderForTab(tab->GetContents()),
                                 command.folder) &&
            command.kind == KR_FOLDER_DELETE)
          close.push_back(Identity(*tab));
      std::erase_if(catalog.items, [&](const TabFolder& value) {
        return folders_.IsDescendant(value.id, command.folder);
      });
      break;
    default:
      return KR_ERR_INVALID_ARGUMENT;
  }
  for (const auto& folder : catalog.items)
    if (!workspaces_.Contains(folder.space))
      return KR_ERR_INVALID_ARGUMENT;
  if (!WriteFolders(*browser_->GetProfile()->GetPrefs(), catalog))
    return KR_ERR_INVALID_ARGUMENT;
  if (created && command.tab)
    if (auto* tab = FindTab(command.tab))
      AssignFolder(*tab, created);
  for (const auto tab : close)
    CloseTab(tab);
  return KR_OK;
}
}  // namespace krypton_ui
