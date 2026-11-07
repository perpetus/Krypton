// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include <algorithm>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/content_settings/cookie_settings_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/accelerator_utils.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/krypton_ui/action_menu_model.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/essential_prefs.h"
#include "chrome/browser/ui/krypton_ui/layout_menus.h"
#include "chrome/browser/ui/krypton_ui/layout_prefs.h"
#include "chrome/browser/ui/krypton_ui/multi_page_controller.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "chrome/browser/ui/krypton_ui/native_extensions.h"
#include "chrome/browser/ui/krypton_ui/native_menus.h"
#include "chrome/browser/ui/krypton_ui/sidebar_menu_model.h"
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "chrome/browser/ui/krypton_ui/site_data_menu_model.h"
#include "chrome/browser/ui/krypton_ui/workspace_close.h"
#include "chrome/browser/ui/krypton_ui/workspace_menu.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/toolbar/app_menu_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/site_data/page_specific_site_data_dialog.h"
#include "chrome/grit/generated_resources.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/omnibox/browser/location_bar_model.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/strings/grit/components_strings.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/actions/actions.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"

namespace krypton_ui {
namespace {
BrowserAdapter* WorkspaceAdapter(const TabStripModel& model) {
  const auto* active = model.GetActiveTab();
  auto* browser = active ? active->GetBrowserWindowInterface() : nullptr;
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  return view ? view->krypton_ui() : nullptr;
}
}  // namespace

bool IsWorkspaceCloseCandidate(const TabStripModel& model,
                               const tabs::TabInterface* candidate) {
  const auto* owner = WorkspaceAdapter(model);
  return !owner || !owner->ready() || !candidate ||
         owner->IsWorkspaceCloseCandidate(*candidate);
}

bool NeedsWorkspaceCloseTarget(content::WebContents* closing) {
  const auto* tab = tabs::TabInterface::MaybeGetFromContents(closing);
  auto* browser = tab ? tab->GetBrowserWindowInterface() : nullptr;
  const auto* owner =
      browser ? WorkspaceAdapter(*browser->GetTabStripModel()) : nullptr;
  return owner && owner->NeedsWorkspaceCloseTarget(*tab);
}

void PrepareWorkspaceCloseTarget(content::WebContents* closing) {
  auto* tab = tabs::TabInterface::MaybeGetFromContents(closing);
  auto* browser = tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (auto* owner =
          browser ? WorkspaceAdapter(*browser->GetTabStripModel()) : nullptr) {
    owner->PrepareWorkspaceCloseTarget(*tab);
  }
}

bool BrowserAdapter::IsWorkspaceCloseCandidate(
    const tabs::TabInterface& tab) const {
  return EssentialForTab(tab.GetContents()) ||
         WorkspaceForTab(tab.GetContents()) == active_space_;
}

bool BrowserAdapter::NeedsWorkspaceCloseTarget(
    const tabs::TabInterface& closing) const {
  if (closed_ || !ready() || !closing.IsActivated() ||
      IsTransientGlance(closing.GetContents())) {
    return false;
  }
  const auto* model = browser_->GetTabStripModel();
  if (model->count() <= 1) {
    return false;  // Chromium still closes the final window tab normally.
  }
  for (const auto* tab : *model) {
    if (tab != &closing && IsWorkspaceCloseCandidate(*tab)) {
      return false;
    }
  }
  return true;
}

void BrowserAdapter::PrepareWorkspaceCloseTarget(tabs::TabInterface& closing) {
  if (!NeedsWorkspaceCloseTarget(closing)) {
    return;
  }
  const base::AutoReset<KrSpaceId> workspace(&new_tab_space_, active_space_);
  // Keep the closing page selected until Chromium removes it. Native close
  // selection then activates this tab through its normal Store/RestoreFocus.
  chrome::AddTabAt(&*browser_, GURL(), -1, /*foreground=*/false);
}
void BrowserAdapter::PublishWorkspaces() {
  const auto* api = session_.api();
  if (!api) {
    return;
  }
  std::vector<KrSpace> spaces;
  spaces.reserve(workspaces_.spaces.size());
  for (const auto& space : workspaces_.spaces) {
    const auto color =
        space.color ? space.color
                    : view_->GetColorProvider()->GetColor(ui::kColorAccent);
    spaces.push_back({sizeof(KrSpace),
                      space.id,
                      {space.name.data(), space.name.size()},
                      {space.glyph.data(), space.glyph.size()},
                      color});
  }
  api->set_spaces(api->self, spaces.data(), spaces.size(), sizeof(KrSpace),
                  active_space_);
}
void BrowserAdapter::AssignWorkspace(tabs::TabInterface& tab, KrSpaceId space) {
  if (const auto* folder = folders_.Find(FolderForTab(tab.GetContents()));
      folder && folder->space != space) {
    AssignFolder(tab, 0);
  }
  if (multi_page_ && !multi_page_->editing_group() &&
      multi_page_->MoveGroup(Identity(tab), space)) {
    return;
  }
  SetWorkspaceForTab(tab.GetContents(), space);
  if (auto* service =
          SessionServiceFactory::GetForProfile(browser_->GetProfile())) {
    const auto id = sessions::SessionTabHelper::IdForTab(tab.GetContents());
    if (id.is_valid()) {
      service->AddTabExtraData(browser_->GetSessionID(), id,
                               kWorkspaceSessionKey,
                               base::NumberToString(space));
    }
  }
}
void BrowserAdapter::EnsureWorkspace(tabs::TabInterface& tab) {
  auto space = WorkspaceForTab(tab.GetContents());
  if (!space) {
    space = new_tab_space_ ? new_tab_space_ : active_space_;
    const auto index = browser_->GetTabStripModel()->GetIndexOfTab(&tab);
    if (!new_tab_space_ && index != TabStripModel::kNoTab) {
      if (const auto* opener =
              browser_->GetTabStripModel()->GetOpenerOfTabAt(index)) {
        const auto opener_space = WorkspaceForTab(opener->GetContents());
        if (!EssentialForTab(opener->GetContents()) &&
            workspaces_.Contains(opener_space)) {
          space = opener_space;
        }
      }
    }
  }
  if (!workspaces_.Contains(space)) {
    space = kDefaultWorkspace;
  }
  AssignWorkspace(tab, space);
}

void BrowserAdapter::AssignEssential(tabs::TabInterface& tab, uint64_t id) {
  SetEssentialForTab(tab.GetContents(), id);
  if (auto* service =
          SessionServiceFactory::GetForProfile(browser_->GetProfile())) {
    const auto session =
        sessions::SessionTabHelper::IdForTab(tab.GetContents());
    if (session.is_valid()) {
      service->AddTabExtraData(browser_->GetSessionID(), session,
                               kEssentialSessionKey, base::NumberToString(id));
    }
  }
}

void BrowserAdapter::SetEssential(tabs::TabInterface& tab, bool enabled) {
  if (enabled && multi_page_ && multi_page_->Describe(Identity(tab))) {
    return;
  }
  if (browser_->GetProfile()->IsOffTheRecord() ||
      tab.GetBrowserWindowInterface() != &*browser_) {
    return;
  }
  const auto icon = site_customization_ ? site_customization_->Glyph(tab)
                                        : TabIconForTab(tab.GetContents());
  const auto previous = EssentialForTab(tab.GetContents());
  if (enabled && FolderForTab(tab.GetContents())) {
    AssignFolder(tab, 0);
  }
  if ((previous != 0) == enabled) {
    return;
  }
  auto& prefs = *browser_->GetProfile()->GetPrefs();
  if (enabled) {
    const auto next = ReadEssentials(prefs).next_id;
    if (!next) {
      return;
    }
    // Mark the real tab before synchronous preference observers restore the
    // catalog in this window and other windows belonging to the profile.
    AssignEssential(tab, next);
    const auto saved =
        SaveEssential(prefs, 0, tab.GetContents()->GetVisibleURL().spec());
    if (!saved) {
      AssignEssential(tab, 0);
    }
  } else {
    AssignEssential(tab, 0);
    std::ignore = RemoveEssential(prefs, previous);
  }
  if (!icon.empty() && site_customization_) {
    site_customization_->SetTabIcon(Identity(tab), icon);
  }
  PublishTab(tab);
  PublishSelection();
}

void BrowserAdapter::OnEssentialsChanged() {
  if (closed_ || restoring_essentials_ ||
      browser_->GetProfile()->IsOffTheRecord() ||
      SessionRestore::IsRestoring(browser_->GetProfile())) {
    return;
  }
  const base::AutoReset<bool> restoring(&restoring_essentials_, true);
  menus_->CloseAll();
  const auto catalog = ReadEssentials(*browser_->GetProfile()->GetPrefs());
  std::vector<uint64_t> present;
  for (auto* tab : *browser_->GetTabStripModel()) {
    const auto id = EssentialForTab(tab->GetContents());
    if (!id) {
      continue;
    }
    if (std::ranges::none_of(
            catalog.items,
            [id](const auto& entry) { return entry.id == id; }) ||
        std::ranges::contains(present, id)) {
      AssignEssential(*tab, 0);
      PublishTab(*tab);
    } else {
      present.push_back(id);
    }
  }
  for (const auto& entry : catalog.items) {
    if (std::ranges::contains(present, entry.id)) {
      continue;
    }
    const GURL url(entry.url);
    bool adopted = false;
    for (auto* tab : *browser_->GetTabStripModel()) {
      if (!EssentialForTab(tab->GetContents()) &&
          !IsTransientGlance(tab->GetContents()) &&
          (!multi_page_ || !multi_page_->Describe(Identity(*tab))) &&
          tab->GetContents()->GetVisibleURL() == url) {
        AssignEssential(*tab, entry.id);
        PublishTab(*tab);
        adopted = true;
        break;
      }
    }
    if (adopted) {
      continue;
    }
    const base::AutoReset<uint64_t> essential(&new_essential_, entry.id);
    const base::AutoReset<KrSpaceId> workspace(&new_tab_space_, active_space_);
    NavigateParams params(&*browser_, url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
    params.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    params.user_gesture = false;
    Navigate(&params);
  }
  PublishSelection();
}

void BrowserAdapter::UpdateEssential(tabs::TabInterface& tab) {
  const auto id = EssentialForTab(tab.GetContents());
  if (id && !restoring_essentials_ &&
      !browser_->GetProfile()->IsOffTheRecord()) {
    std::ignore = SaveEssential(*browser_->GetProfile()->GetPrefs(), id,
                                tab.GetContents()->GetVisibleURL().spec());
  }
}
void BrowserAdapter::OnWorkspacesChanged() {
  menus_->CloseAll();
  workspaces_ = ReadWorkspaces(*browser_->GetProfile()->GetPrefs());
  if (CanFolders()) {
    auto folders = ReadFolders(*browser_->GetProfile()->GetPrefs());
    bool changed = false;
    for (auto& folder : folders.items) {
      if (!workspaces_.Contains(folder.space)) {
        folder.space = kDefaultWorkspace;
        changed = true;
      }
    }
    if (changed) {
      std::ignore = WriteFolders(*browser_->GetProfile()->GetPrefs(), folders);
    }
  }
  workspace_themes_.Retain(workspaces_);
  if (!workspaces_.Contains(active_space_)) {
    active_space_ = kDefaultWorkspace;
    RememberWorkspace(*browser_->GetProfile()->GetPrefs(), active_space_);
  }
  for (auto* tab : *browser_->GetTabStripModel()) {
    if (!workspaces_.Contains(WorkspaceForTab(tab->GetContents()))) {
      AssignWorkspace(*tab, kDefaultWorkspace);
      PublishTab(*tab);
    }
  }
  PublishWorkspaces();
  PublishTheme();
}
void BrowserAdapter::SwitchWorkspace(KrSpaceId space) {
  if (!workspaces_.Contains(space)) {
    return;
  }
  menus_->CloseAll();
  if (multi_page_ && !multi_page_->BeforeWorkspaceChange(space)) {
    return;
  }
  active_space_ = space;
  RememberWorkspace(*browser_->GetProfile()->GetPrefs(), space);
  PublishWorkspaces();
  auto* model = browser_->GetTabStripModel();
  auto* selected = browser_->GetActiveTabInterface();
  if (selected && (EssentialForTab(selected->GetContents()) ||
                   WorkspaceForTab(selected->GetContents()) == space)) {
    return;
  }
  tabs::TabInterface* target = nullptr;
  for (auto* tab : *model) {
    if (WorkspaceForTab(tab->GetContents()) == space &&
        (!target || tab->GetContents()->GetLastActiveTime() >
                        target->GetContents()->GetLastActiveTime())) {
      target = tab;
    }
  }
  if (target) {
    model->ActivateTabAt(model->GetIndexOfTab(target));
  } else {
    // A workspace always has a real foreground tab. This also makes Chromium's
    // selected-tab session record sufficient to restore the active workspace.
    const base::AutoReset<KrSpaceId> creating(&new_tab_space_, space);
    chrome::NewTab(&*browser_, NewTabTypes::kNewTabCommand);
  }
}
void BrowserAdapter::EditWorkspace(uint32_t kind, Workspace value) {
  auto& prefs = *browser_->GetProfile()->GetPrefs();
  if (kind == KR_WORKSPACE_REMOVE) {
    // The synchronous pref notification migrates affected tabs in all windows.
    std::ignore = RemoveWorkspace(prefs, value.id);
  } else if (kind == KR_WORKSPACE_UPSERT) {
    const bool create = !value.id;
    const auto id = SaveWorkspace(prefs, std::move(value));
    if (create && id) {
      SwitchWorkspace(*id);
    }
  }
}
std::unique_ptr<ui::MenuModel> BrowserAdapter::CreateWorkspaceMenu(
    KrSpaceId target) {
  auto* tab = browser_->GetActiveTabInterface();
  if (multi_page_) {
    tab = multi_page_->PresentedTab(tab);
  }
  return std::make_unique<WorkspaceMenuModel>(
      workspaces_, active_space_, target, tab != nullptr,
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner,
             base::WeakPtr<tabs::TabInterface> tab,
             WorkspaceMenuModel::Operation operation, KrSpaceId space) {
            if (!owner) {
              return;
            }
            switch (operation) {
              case WorkspaceMenuModel::Operation::kSwitch:
                owner->SwitchWorkspace(space);
                break;
              case WorkspaceMenuModel::Operation::kCreate:
              case WorkspaceMenuModel::Operation::kEdit:
                if (!space || owner->workspaces_.Contains(space)) {
                  if (const auto* api = owner->session_.api();
                      KR_HAS_SLOT(api, KrShellUI, show_workspace_editor) &&
                      api->show_workspace_editor) {
                    std::ignore = api->show_workspace_editor(api->self, space);
                  }
                }
                break;
              case WorkspaceMenuModel::Operation::kMoveTab:
                if (tab &&
                    tab.get() ==
                        (owner->multi_page_
                             ? owner->multi_page_->PresentedTab(
                                   owner->browser_->GetActiveTabInterface())
                             : owner->browser_->GetActiveTabInterface()) &&
                    owner->workspaces_.Contains(space)) {
                  owner->MoveTab(
                      Identity(*tab), space,
                      base::checked_cast<size_t>(
                          owner->browser_->GetTabStripModel()->GetIndexOfTab(
                              tab.get())));
                }
                break;
              case WorkspaceMenuModel::Operation::kTheme:
                owner->ShowThemeEditor(space);
                break;
            }
          },
          weak_factory_.GetWeakPtr(),
          tab ? tab->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>()),
      !browser_->GetProfile()->IsOffTheRecord());
}
std::unique_ptr<ui::MenuModel> BrowserAdapter::CreateContextMenu(
    uint32_t kind,
    tabs::TabInterface* target) {
  if (!target || target->GetBrowserWindowInterface() != &*browser_) {
    return nullptr;
  }
  auto tab = target->GetWeakPtr();
  auto owner = weak_factory_.GetWeakPtr();
  if (kind == KR_MENU_SITE_DATA) {
    return CreateSiteDataMenu(*target);
  }
  if (kind == KR_MENU_TAB) {
    auto can_move = base::BindRepeating(
        [](base::WeakPtr<BrowserAdapter> owner,
           base::WeakPtr<tabs::TabInterface> tab, KrSpaceId space) {
          return owner && tab &&
                 tab->GetBrowserWindowInterface() == &*owner->browser_ &&
                 owner->workspaces_.Contains(space) &&
                 WorkspaceForTab(tab->GetContents()) != space;
        },
        owner, tab);
    return std::make_unique<WorkspaceTabMenuModel>(
        workspaces_, WorkspaceForTab(target->GetContents()), can_move,
        base::BindRepeating(
            [](base::WeakPtr<BrowserAdapter> owner,
               base::WeakPtr<tabs::TabInterface> tab,
               WorkspaceTabMenuModel::CanMove can_move, KrSpaceId space) {
              if (!can_move.Run(space)) {
                return;
              }
              owner->MoveTab(
                  Identity(*tab), space,
                  base::checked_cast<size_t>(
                      owner->browser_->GetTabStripModel()->GetIndexOfTab(
                          tab.get())));
            },
            owner, tab, can_move));
  }
  if (kind != KR_MENU_SIDEBAR) {
    return nullptr;
  }
  std::vector<SidebarMenuModel::Action> actions;
  const auto add = [&](int command, actions::ActionId id, int label) {
    auto* action = actions::ActionManager::Get().FindAction(
        id, BrowserActions::From(&*browser_)->root_action_item());
    if (action) {
      actions.push_back(
          {command, action->GetAsWeakPtr(), l10n_util::GetStringUTF16(label)});
    }
  };
  add(IDC_NEW_TAB, kActionNewTab, IDS_NEW_TAB);
  add(IDC_RELOAD, kActionReload, IDS_TAB_CXMENU_RELOAD);
  add(IDC_BOOKMARK_THIS_TAB, kActionBookmarkThisTab, IDS_BOOKMARK_THIS_TAB);
  add(IDC_RESTORE_TAB, kActionRestoreTab, IDS_RESTORE_TAB);
  auto read_state = base::BindRepeating(
      [](base::WeakPtr<BrowserAdapter> owner,
         base::WeakPtr<tabs::TabInterface> tab)
          -> std::optional<SidebarMenuModel::State> {
        if (!owner || !tab ||
            owner->browser_->GetActiveTabInterface() != tab.get()) {
          return std::nullopt;
        }
        const auto* api = owner->session_.api();
        return SidebarMenuModel::State{
            ReadLayoutPrefs(*owner->browser_->GetProfile()->GetPrefs()).flags,
            KR_HAS_SLOT(api, KrShellUI, show_workspace_editor) &&
                api->show_workspace_editor,
            api && api->show_settings,
            api && KR_HAS_SLOT(api, KrShellUI, show_theme_editor) &&
                api->show_theme_editor &&
                !owner->browser_->GetProfile()->IsOffTheRecord()};
      },
      owner, tab);
  return std::make_unique<SidebarMenuModel>(
      actions, read_state,
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner, int command) {
            return owner &&
                   chrome::IsCommandEnabled(&*owner->browser_, command);
          },
          owner),
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner,
             SidebarMenuModel::ReadState read_state,
             SidebarMenuModel::Command command) {
            if (!read_state.Run()) {
              return;
            }
            auto* prefs = owner->browser_->GetProfile()->GetPrefs();
            const auto* api = owner->session_.api();
            switch (command) {
              case SidebarMenuModel::kCompact:
              case SidebarMenuModel::kSidebarRight: {
                auto layout = ReadLayoutPrefs(*prefs);
                layout.flags = ApplyLayoutCommand(
                    layout.flags, command == SidebarMenuModel::kCompact
                                      ? kCompactMode
                                      : kSidebarRight);
                WriteLayoutPrefs(*prefs, layout);
                break;
              }
              case SidebarMenuModel::kNewWorkspace:
                if (KR_HAS_SLOT(api, KrShellUI, show_workspace_editor) &&
                    api->show_workspace_editor) {
                  std::ignore = api->show_workspace_editor(api->self, 0);
                }
                break;
              case SidebarMenuModel::kAppearance:
                if (api && api->show_settings) {
                  std::ignore =
                      api->show_settings(api->self, KR_SETTINGS_APPEARANCE);
                }
                break;
              case SidebarMenuModel::kEditTheme:
                owner->ShowThemeEditor(owner->active_space_);
                break;
            }
          },
          owner, read_state));
}
std::unique_ptr<ui::MenuModel> BrowserAdapter::CreateSiteDataMenu(
    tabs::TabInterface& target) {
  auto owner = weak_factory_.GetWeakPtr();
  auto tab = target.GetWeakPtr();
  const auto url = target.GetContents()->GetVisibleURL();
  auto state = base::BindRepeating(
      [](base::WeakPtr<BrowserAdapter> owner,
         base::WeakPtr<tabs::TabInterface> tab,
         const GURL& url) -> std::optional<SiteDataMenuModel::State> {
        if (!owner || !tab || owner->closed_ ||
            owner->browser_->GetActiveTabInterface() != tab.get() ||
            tab->GetContents()->GetVisibleURL() != url) {
          return std::nullopt;
        }
        SiteDataMenuModel::State state;
        state.site_available = url.SchemeIsHTTPOrHTTPS();
        state.secure = owner->security_ == KR_SECURITY_SECURE;
        if (owner->site_customization_) {
          const auto boost = owner->site_customization_->BoostForSite(url);
          state.boost_available = boost.available;
        }
        state.security_label =
            LocationBarModel::From(&*owner->browser_)->GetSecureDisplayText();
        if (state.security_label.empty()) {
          state.security_label = l10n_util::GetStringUTF16(
              state.secure ? IDS_PAGE_INFO_SECURE_SUMMARY
                           : IDS_TOOLTIP_LOCATION_ICON);
        }
        if (state.site_available) {
          auto cookies = CookieSettingsFactory::GetForProfile(
              owner->browser_->GetProfile());
          if (cookies) {
            state.protection_status = l10n_util::GetStringUTF16(
                cookies->IsThirdPartyAccessAllowed(url)
                    ? IDS_COOKIE_CONTROLS_BUBBLE_COOKIES_ALLOWED_TITLE
                    : IDS_COOKIE_CONTROLS_BUBBLE_COOKIES_BLOCKED_TITLE);
          }
        }
        return state;
      },
      owner, tab, url);
  std::vector<SiteDataMenuModel::Action> actions;
  const auto add = [&](int command, actions::ActionId id, int label) {
    auto* action = actions::ActionManager::Get().FindAction(
        id, BrowserActions::From(&*browser_)->root_action_item());
    if (action) {
      actions.push_back(
          {command, action->GetAsWeakPtr(), l10n_util::GetStringUTF16(label)});
    }
  };
  add(IDC_BOOKMARK_THIS_TAB, kActionBookmarkThisTab, IDS_BOOKMARK_THIS_TAB);
  add(IDC_SHARING_HUB, kActionSharingHub, IDS_SHARING_HUB_TOOLTIP);
  auto browser_tools = std::make_unique<AppMenuModel>(
      AcceleratorProviderForBrowser(&*browser_), &*browser_);
  browser_tools->Init();
  std::unique_ptr<ui::MenuModel> more = std::move(browser_tools);
  if (site_customization_) {
    std::vector<ActionMenuModel::Submenu> extras;
    extras.push_back(
        {0x6d620003, u"Workspaces", CreateWorkspaceMenu(active_space_)});
    extras.push_back(
        {0x6d620001, u"Workspace routing", site_customization_->RoutingMenu()});
    extras.push_back(
        {0x6d620002, u"Site boosts", site_customization_->BoostMenu()});
    more =
        std::make_unique<ActionMenuModel>(std::move(more), std::move(extras));
  }
  return std::make_unique<SiteDataMenuModel>(
      actions,
      extensions_ ? extensions_->CreateMenu(NativeExtensions::kMenuAction, true)
                  : nullptr,
      std::move(more),
      SiteDataMenuModel::Labels{
          l10n_util::GetStringUTF16(IDS_EXTENSIONS_MENU_TITLE),
          l10n_util::GetStringUTF16(IDS_SETTINGS),
          l10n_util::GetStringUTF16(
              IDS_COOKIE_CONTROLS_BUBBLE_THIRD_PARTY_COOKIES_LABEL),
          l10n_util::GetStringUTF16(IDS_PAGE_INFO_SITE_SETTINGS_LINK),
          l10n_util::GetStringUTF16(IDS_MORE_TOOLS_MENU),
          l10n_util::GetStringUTF16(IDS_TAKE_SCREENSHOT), u"Boost"},
      state,
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner, int command) {
            if (command == SiteDataMenuModel::kBoosts) {
              const auto* api = owner ? owner->session_.api() : nullptr;
              const auto* tab =
                  owner ? owner->browser_->GetActiveTabInterface() : nullptr;
              return owner && owner->site_customization_ && tab &&
                     tab->GetContents()
                         ->GetLastCommittedURL()
                         .SchemeIsHTTPOrHTTPS() &&
                     !owner->browser_->GetProfile()->IsOffTheRecord() && api &&
                     KR_HAS_SLOT(api, KrShellUI, show_boost_editor) &&
                     api->show_boost_editor;
            }
            if (command == SiteDataMenuModel::kScreenshot) {
              return owner && owner->CanTakeScreenshot();
            }
            return owner &&
                   chrome::IsCommandEnabled(&*owner->browser_, command);
          },
          owner),
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner,
             base::WeakPtr<tabs::TabInterface> tab,
             SiteDataMenuModel::ReadState state,
             SiteDataMenuModel::Command command) {
            if (!state.Run()) {
              return;
            }
            switch (command) {
              case SiteDataMenuModel::kBoosts:
                std::ignore = owner->session_.Defer(base::BindOnce(
                    [](base::WeakPtr<BrowserAdapter> owner,
                       SiteDataMenuModel::ReadState state) {
                      if (owner && state.Run() && owner->site_customization_) {
                        owner->site_customization_->OpenBoost();
                      }
                    },
                    owner, state));
                break;
              case SiteDataMenuModel::kScreenshot:
                owner->TakeScreenshot(tab.get());
                break;
              case SiteDataMenuModel::kSecurity:
              case SiteDataMenuModel::kSecureConnection: {
                auto result = owner->ShowPageInfo(tab.get());
                if (!result.has_value()) {
                  LOG(ERROR) << result.error();
                }
                break;
              }
              case SiteDataMenuModel::kSiteSettings:
                chrome::ShowSiteSettings(&*owner->browser_,
                                         tab->GetContents()->GetVisibleURL());
                break;
              case SiteDataMenuModel::kProtection:
                ShowPageSpecificSiteDataDialog(tab->GetContents());
                break;
              default:
                break;
            }
          },
          owner, tab, state),
      site_customization_ ? site_customization_->SiteBoostsMenu() : nullptr);
}

uint32_t BrowserAdapter::ThemeScheme() const {
  return workspace_themes_.preview_scheme().value_or(static_cast<uint32_t>(
      ThemeServiceFactory::GetForProfile(browser_->GetProfile())
          ->GetBrowserColorScheme()));
}

void BrowserAdapter::PublishWorkspaceThemes() {
  const auto* api = session_.api();
  if (!KR_HAS_SLOT(api, KrShellUI, set_workspace_theme) ||
      !api->set_workspace_theme || closed_) {
    return;
  }
  const auto scheme = ThemeScheme();
  const auto& prefs = *browser_->GetProfile()->GetPrefs();
  for (const auto& space : workspaces_.spaces) {
    const auto theme = workspace_themes_.Resolve(prefs, space.id, scheme);
    std::ignore = api->set_workspace_theme(api->self, &theme);
  }
}

void BrowserAdapter::ApplyWorkspaceTheme(const KrWorkspaceTheme& theme,
                                         bool commit) {
  const auto* api = session_.api();
  if (closed_ || browser_->GetProfile()->IsOffTheRecord() ||
      !workspaces_.Contains(theme.space) || !ValidWorkspaceTheme(theme) ||
      !KR_HAS_SLOT(api, KrShellUI, set_workspace_theme) ||
      !api->set_workspace_theme) {
    return;
  }
  // The module remains the authority for editor constraints and token ranges.
  if (api->set_workspace_theme(api->self, &theme) != KR_OK) {
    PublishWorkspaceThemes();
    return;
  }
  const auto previous_scheme = ThemeScheme();
  if (!workspace_themes_.Apply(*browser_->GetProfile()->GetPrefs(), theme,
                               commit)) {
    PublishWorkspaceThemes();
    return;
  }
  if (commit) {
    auto* service = ThemeServiceFactory::GetForProfile(browser_->GetProfile());
    const auto scheme =
        static_cast<ThemeService::BrowserColorScheme>(theme.scheme);
    if (service->GetBrowserColorScheme() != scheme) {
      service->SetBrowserColorScheme(scheme);
    }
  }
  PublishWorkspaceThemes();
  if (previous_scheme != ThemeScheme()) {
    PublishTheme();
  }
}

void BrowserAdapter::ShowThemeEditor(KrSpaceId space) {
  if (!space) {
    space = active_space_;
  }
  const auto* api = session_.api();
  if (closed_ || browser_->GetProfile()->IsOffTheRecord() ||
      !workspaces_.Contains(space) ||
      !KR_HAS_SLOT(api, KrShellUI, show_theme_editor) ||
      !api->show_theme_editor) {
    return;
  }
  PublishWorkspaceThemes();
  const auto result = api->show_theme_editor(api->self, space);
  if (result != KR_OK) {
    LOG(ERROR) << "KryptonUI theme editor failed for workspace " << space
               << ": " << result;
  }
}
}  // namespace krypton_ui
