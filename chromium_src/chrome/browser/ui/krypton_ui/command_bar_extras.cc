// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/krypton_ui/command_bar_entry.h"
#include "chrome/browser/ui/krypton_ui/document_store.h"
#include "chrome/browser/ui/krypton_ui/essential_prefs.h"
#include "chrome/browser/ui/krypton_ui/layout_menus.h"
#include "chrome/browser/ui/krypton_ui/layout_prefs.h"
#include "chrome/browser/ui/krypton_ui/native_extensions.h"
#include "chrome/browser/ui/krypton_ui/native_location_bar.h"
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/image_model.h"
#include "ui/views/widget/widget.h"

namespace krypton_ui {

void BrowserAdapter::ClearTemporaryTabs(KrSpaceId space) {
  if (active_space_ != space) {
    return;
  }
  auto weak = GetWeakPtr();
  std::vector<base::WeakPtr<tabs::TabInterface>> targets;
  for (auto* tab : *browser_->GetTabStripModel()) {
    if (!tab->IsPinned() && !EssentialForTab(tab->GetContents()) &&
        WorkspaceForTab(tab->GetContents()) == space) {
      targets.push_back(tab->GetWeakPtr());
    }
  }
  for (auto target : targets) {
    if (!weak) {
      return;
    }
    if (target && !target->IsPinned() &&
        !EssentialForTab(target->GetContents()) &&
        WorkspaceForTab(target->GetContents()) == space) {
      browser_->GetTabStripModel()->CloseWebContents(
          target->GetContents(), TabCloseTypes::CLOSE_USER_GESTURE);
    }
  }
}

void BrowserAdapter::RecordPinnedHome(tabs::TabInterface& tab) {
  auto* contents = tab.GetContents();
  const auto previous = PinnedHomeForTab(contents);
  if (tab.IsPinned() && !previous.empty())
    return;
  const auto url =
      tab.IsPinned() ? contents->GetLastCommittedURL().spec() : std::string();
  if (url == previous || url.size() > 65536)
    return;
  SetPinnedHomeForTab(contents, url);
  if (auto* service =
          SessionServiceFactory::GetForProfile(browser_->GetProfile()))
    service->AddTabExtraData(browser_->GetSessionID(),
                             sessions::SessionTabHelper::IdForTab(contents),
                             kPinnedHomeSessionKey, url);
}
void BrowserAdapter::AppendExtraCommands(
    std::string_view query,
    std::vector<CommandBarEntry>& entries) {
  const auto owner = weak_factory_.GetWeakPtr();
  auto* active = browser_->GetActiveTabInterface();
  const auto tab =
      active ? active->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>();
  const KrSpaceId space = active_space_;
  const auto add = [&](std::string title, std::string_view aliases,
                       base::OnceClosure execute) {
    if (execute && MatchesCommand(query, title, aliases))
      entries.push_back(
          {std::move(title), "Browser command", std::move(execute)});
  };
  if (CanCreateDocument()) {
    add("New Note", "create write markdown document",
        base::BindOnce(&BrowserAdapter::NewDocument, owner,
                       DocumentKind::kNote));
    add("New Easel", "create draw canvas whiteboard",
        base::BindOnce(&BrowserAdapter::NewDocument, owner,
                       DocumentKind::kEasel));
  }
  add("New Space", "create workspace profile theme",
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner) {
            if (!owner || owner->browser_->GetProfile()->IsOffTheRecord())
              return;
            const auto* api = owner->session_.api();
            if (api && KR_HAS_SLOT(api, KrShellUI, show_workspace_editor) &&
                api->show_workspace_editor) {
              auto* frame =
                  owner->frame_adapter() ? owner->frame_adapter() : owner.get();
              frame->customize_new_space_ = true;
              std::ignore = api->show_workspace_editor(api->self, 0);
            }
          },
          owner));
  add("Customize Space theme", "edit appearance color gradient",
      base::BindOnce(&BrowserAdapter::ShowThemeEditor, owner, space));
  for (const auto& [scheme, title] :
       {std::pair{KR_COLOR_DARK, "Switch to dark mode"},
        std::pair{KR_COLOR_LIGHT, "Switch to light mode"},
        std::pair{KR_COLOR_SYSTEM, "Use system theme"}}) {
    add(title, "appearance color theme",
        base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner, int scheme) {
              if (owner)
                owner->WindowPrefs().SetInteger(kColorSchemePref, scheme);
            },
            owner, scheme));
  }
  add("Toggle sidebar", "hide show compact",
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner) {
            if (!owner)
              return;
            auto layout = ReadLayoutPrefs(owner->WindowPrefs());
            layout.flags = ApplyLayoutCommand(layout.flags, kCompactMode);
            WriteLayoutPrefs(owner->WindowPrefs(), layout);
          },
          owner));
  add("Minimize window", "minimise",
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner) {
            if (owner && owner->FrameWidget())
              owner->FrameWidget()->Minimize();
          },
          owner));
  add("Close window", "quit frame",
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner) {
            if (!owner)
              return;
            if (auto* frame = owner->frame_adapter())
              frame->CloseFrameWindow();
            else
              owner->CloseFrameWindow();
          },
          owner));
  add("New blank window", "empty",
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner) {
            if (!owner ||
                !chrome::IsCommandEnabled(&*owner->browser_, IDC_NEW_WINDOW))
              return;
            NavigateParams params(&*owner->browser_, GURL("about:blank"),
                                  ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
            params.disposition = WindowOpenDisposition::kNewWindow;
            Navigate(&params);
          },
          owner));
  add("Add split view", "split tabs columns rows grid",
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner) {
            if (owner)
              std::ignore = owner->ShowSplitChooser();
          },
          owner));
  if (CanFolders()) {
    add("New folder", "create organize",
        base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner, KrSpaceId space) {
              if (!owner || owner->active_space_ != space)
                return;
              KrFolderCommand command{};
              command.struct_size = sizeof(command);
              command.kind = KR_FOLDER_CREATE;
              command.space = space;
              std::ignore = owner->FolderCommand(command);
            },
            owner, space));
  }
  if (active) {
    if (!browser_->GetProfile()->IsOffTheRecord()) {
      const bool essential = EssentialForTab(active->GetContents()) != 0;
      add(essential ? "Remove from Essentials"
                    : "Favorite tab (add to Essentials)",
          "essential favorite favourite tab",
          base::BindOnce(
              [](base::WeakPtr<BrowserAdapter> owner,
                 base::WeakPtr<tabs::TabInterface> tab, bool enabled) {
                if (owner && tab &&
                    owner->browser_->GetActiveTabInterface() == tab.get())
                  owner->SetEssential(*tab, enabled);
              },
              owner, tab, !essential));
    }
    add("Reveal tab in sidebar", "show locate focus",
        base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner,
               base::WeakPtr<tabs::TabInterface> tab) {
              if (!owner || !tab ||
                  owner->browser_->GetActiveTabInterface() != tab.get())
                return;
              std::vector<KrFolderId> parents;
              auto folder = FolderForTab(tab->GetContents());
              while (folder && parents.size() < 32) {
                const auto* value = owner->folders_.Find(folder);
                if (!value)
                  break;
                parents.push_back(folder);
                folder = value->parent;
              }
              for (const auto id : parents) {
                KrFolderCommand command{};
                command.struct_size = sizeof(command);
                command.kind = KR_FOLDER_COLLAPSE;
                command.folder = id;
                command.collapsed = false;
                std::ignore = owner->FolderCommand(command);
              }
              const auto* api = owner->session_.api();
              const KrNativeFocus target{
                  sizeof(target), KR_ANCHOR_TAB,
                  base::checked_cast<KrTabId>(tab->GetHandle().raw_value())};
              if (api && KR_HAS_SLOT(api, KrShellUI, focus_native) &&
                  api->focus_native)
                std::ignore = api->focus_native(api->self, &target);
            },
            owner, tab));
    add("Copy Markdown link", "clipboard title url address",
        base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner,
               base::WeakPtr<tabs::TabInterface> tab) {
              if (!owner || !tab ||
                  owner->browser_->GetActiveTabInterface() != tab.get())
                return;
              auto* contents = tab->GetContents();
              if (!chrome::CanCopyUrl(&*owner->browser_))
                return;
              std::string title = base::UTF16ToUTF8(contents->GetTitle()),
                          escaped;
              for (char c : title) {
                if (c == '\\' || c == '[' || c == ']')
                  escaped += '\\';
                escaped += (c == '\r' || c == '\n') ? ' ' : c;
              }
              ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
              writer.WriteText(
                  base::UTF8ToUTF16("[" + escaped + "](<" +
                                    contents->GetVisibleURL().spec() + ">)"));
            },
            owner, tab));
    add("Paste and go", "clipboard navigate search",
        base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner,
               base::WeakPtr<tabs::TabInterface> tab,
               WindowOpenDisposition disposition) {
              if (!owner || !tab ||
                  owner->browser_->GetActiveTabInterface() != tab.get())
                return;
              ui::Clipboard::GetForCurrentThread()->ReadText(
                  ui::ClipboardBuffer::kCopyPaste, std::nullopt,
                  base::BindOnce(
                      [](base::WeakPtr<BrowserAdapter> owner,
                         base::WeakPtr<tabs::TabInterface> tab,
                         WindowOpenDisposition disposition,
                         std::u16string text) {
                        if (!owner || !tab ||
                            owner->browser_->GetActiveTabInterface() !=
                                tab.get() ||
                            text.empty() || text.size() > 32768)
                          return;
                        auto navigate = owner->location_->CaptureNavigation(
                            base::UTF16ToUTF8(text), disposition);
                        if (navigate)
                          std::ignore =
                              owner->session_.Defer(std::move(navigate));
                      },
                      owner, tab, disposition));
            },
            owner, tab,
            location_->launcher_mode()
                ? WindowOpenDisposition::kNewForegroundTab
                : WindowOpenDisposition::kCurrentTab));
    if (site_customization_ && !browser_->GetProfile()->IsOffTheRecord() &&
        active->GetContents()->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      add("New Boost / edit Boost", "css theme customize site",
          base::BindOnce(
              [](base::WeakPtr<BrowserAdapter> owner,
                 base::WeakPtr<tabs::TabInterface> tab) {
                if (owner && tab &&
                    owner->browser_->GetActiveTabInterface() == tab.get() &&
                    owner->site_customization_)
                  owner->site_customization_->OpenBoost();
              },
              owner, tab));
    }
    if (active->GetContents()->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      add("Open page in Glance", "little arc peek lightweight window",
          base::BindOnce(
              [](base::WeakPtr<BrowserAdapter> owner,
                 base::WeakPtr<tabs::TabInterface> tab) {
                if (!owner || !tab ||
                    owner->browser_->GetActiveTabInterface() != tab.get())
                  return;
                const auto url =
                    tab->GetContents()->GetLastCommittedURL().spec();
                KrMultiPageCommand command{};
                command.struct_size = sizeof(command);
                command.kind = KR_MULTI_PAGE_OPEN_GLANCE;
                command.tab =
                    base::checked_cast<KrTabId>(tab->GetHandle().raw_value());
                command.url = {url.data(), url.size()};
                std::ignore = owner->MultiPageCommand(command);
              },
              owner, tab));
    }
    for (const auto& destination : workspaces_.spaces) {
      if (destination.id == space || !HostsSpace(destination.id))
        continue;
      add("Move to Space: " + destination.name, "file organize tab workspace",
          base::BindOnce(
              [](base::WeakPtr<BrowserAdapter> owner,
                 base::WeakPtr<tabs::TabInterface> tab, KrSpaceId to) {
                if (owner && tab &&
                    owner->browser_->GetActiveTabInterface() == tab.get() &&
                    owner->HostsSpace(to)) {
                  owner->AssignWorkspace(*tab, to);
                  owner->PublishTab(*tab);
                  owner->PublishSelection();
                }
              },
              owner, tab, destination.id));
      add("Pin to Space: " + destination.name, "file organize tab workspace",
          base::BindOnce(
              [](base::WeakPtr<BrowserAdapter> owner,
                 base::WeakPtr<tabs::TabInterface> tab, KrSpaceId to) {
                if (!owner || !tab ||
                    owner->browser_->GetActiveTabInterface() != tab.get() ||
                    !owner->HostsSpace(to))
                  return;
                auto* model = owner->browser_->GetTabStripModel();
                const int index = model->GetIndexOfTab(tab.get());
                if (index < 0)
                  return;
                model->SetTabPinned(index, true);
                owner->AssignWorkspace(*tab, to);
                owner->PublishTab(*tab);
                owner->PublishSelection();
              },
              owner, tab, destination.id));
    }
    if (CanFolders())
      for (const auto& folder : folders_.items) {
        if (folder.space != space)
          continue;
        add("Pin to folder: " + folder.name, "move organize tab",
            base::BindOnce(
                [](base::WeakPtr<BrowserAdapter> owner,
                   base::WeakPtr<tabs::TabInterface> tab, KrFolderId id) {
                  if (!owner || !tab ||
                      owner->browser_->GetActiveTabInterface() != tab.get() ||
                      !owner->folders_.Find(id))
                    return;
                  auto* model = owner->browser_->GetTabStripModel();
                  const int index = model->GetIndexOfTab(tab.get());
                  if (index < 0)
                    return;
                  model->SetTabPinned(index, true);
                  owner->AssignFolder(*tab, id);
                  owner->PublishTab(*tab);
                  owner->PublishSelection();
                },
                owner, tab, folder.id));
      }
  }
  // Snapshot identities before closing; unload handling and recently-closed
  // sessions remain Chromium's. No archive or background cleanup is introduced.
  add("Clear Today Tabs", "close temporary unpinned space",
      base::BindOnce(&BrowserAdapter::ClearTemporaryTabs, owner, space));
  add("Reset Space", "restore pinned original default urls",
      base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner, KrSpaceId space) {
            if (!owner || owner->active_space_ != space)
              return;
            const auto essentials =
                ReadEssentials(*owner->browser_->GetProfile()->GetPrefs());
            std::vector<std::pair<base::WeakPtr<tabs::TabInterface>, GURL>>
                targets;
            for (auto* tab : *owner->browser_->GetTabStripModel()) {
              if (!tab->IsPinned() ||
                  WorkspaceForTab(tab->GetContents()) != space)
                continue;
              auto* contents = tab->GetContents();
              GURL url;
              for (const auto& item : essentials.items)
                if (item.id == EssentialForTab(contents))
                  url = GURL(item.url);
              // Chromium preserves each tab's original committed navigation.
              // Older non-Essential pins without one are left alone.
              if (url.is_empty())
                url = GURL(PinnedHomeForTab(contents));
              if (url.is_empty())
                if (auto* entry = contents->GetController().GetEntryAtIndex(0))
                  url = entry->GetURL();
              if (url.is_valid() && url.SchemeIsHTTPOrHTTPS())
                targets.emplace_back(tab->GetWeakPtr(), url);
            }
            for (const auto& [target, url] : targets) {
              if (!owner)
                return;
              if (target && target->IsPinned() &&
                  WorkspaceForTab(target->GetContents()) == space) {
                content::NavigationController::LoadURLParams params(url);
                target->GetContents()->GetController().LoadURLWithParams(
                    params);
              }
            }
          },
          owner, space));
  if (extensions_)
    extensions_->AppendCommandEntries(query, entries);

  // Only browsers in this catalog (or this exact profile) contribute tab
  // metadata. History, bookmarks and extensions never cross profile boundaries.
  if (!query.empty())
    for (auto* window : GetAllBrowserWindowInterfaces()) {
      auto* view = BrowserView::GetBrowserViewForBrowser(window);
      auto* adapter = view ? view->krypton_ui() : nullptr;
      if (!adapter || !adapter->browser_initialized() || adapter->closed_ ||
          &adapter->CatalogProfile() != &CatalogProfile())
        continue;
      for (auto* target : *window->GetTabStripModel()) {
        // Switching to the tab already in front would do nothing.
        if (target == browser_->GetActiveTabInterface())
          continue;
        auto* contents = target->GetContents();
        const KrSpaceId tab_space = WorkspaceForTab(contents);
        const auto* workspace = FindSpace(tab_space);
        std::string title = base::UTF16ToUTF8(contents->GetTitle());
        const std::string url = contents->GetVisibleURL().spec();
        if (!MatchesCommand(query, title,
                            url + " " + (workspace ? workspace->name : "")))
          continue;
        if (title.empty())
          title = url;
        // The same icon the sidebar shows for this tab.
        ui::ImageModel icon;
        if (auto* driver =
                favicon::ContentFaviconDriver::FromWebContents(contents))
          icon = ui::ImageModel::FromImage(driver->GetFavicon());
        if (adapter->site_customization_)
          icon = adapter->site_customization_->Icon(*target, icon);
        entries.push_back(
            {std::move(title),
             workspace && tab_space != active_space_
                 ? "Switch to Tab · " + workspace->name
                 : std::string("Switch to Tab"),
             base::BindOnce(
                 [](base::WeakPtr<BrowserAdapter> adapter,
                    base::WeakPtr<tabs::TabInterface> tab) {
                   if (!adapter || !tab)
                     return;
                   adapter->SwitchWorkspace(
                       WorkspaceForTab(tab->GetContents()));
                   if (!adapter || !tab)
                     return;
                   adapter->browser_->GetTabStripModel()->ActivateTab(
                       tab.get());
                   if (adapter->FrameWidget())
                     adapter->FrameWidget()->Activate();
                 },
                 adapter->GetWeakPtr(), target->GetWeakPtr()),
             KR_SUGGESTION_OPEN_TAB, 0, std::move(icon),
             contents->GetVisibleURL()});
      }
    }
}
}  // namespace krypton_ui
