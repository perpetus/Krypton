// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/native_menus.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/extensions/context_menu_matcher.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/accelerator_utils.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/action_menu_model.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/layout_prefs.h"
#include "chrome/browser/ui/krypton_ui/menu_glyphs.h"
#include "chrome/browser/ui/krypton_ui/multi_page_menu.h"
#include "chrome/browser/ui/krypton_ui/native_extensions.h"
#include "chrome/browser/ui/krypton_ui/sidebar_menu_model.h"
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "chrome/browser/ui/krypton_ui/site_data_menu_model.h"
#include "chrome/browser/ui/krypton_ui/site_info_menu.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/krypton_ui/workspace_menu.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/tabs/tab_menu_model.h"
#include "chrome/browser/ui/tabs/tab_menu_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/toolbar/app_menu_model.h"
#include "chrome/browser/ui/toolbar/back_forward_menu_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/toolbar/pinned_action_toolbar_button_menu_model.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/grit/generated_resources.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/window_open_disposition_utils.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
namespace {
constexpr int kChangeTabIcon = 0x6d620000;
constexpr int kRoutingMenu = 0x6d620001;
constexpr int kBoostMenu = 0x6d620002;
constexpr int kWorkspacesMenu = 0x6d620003;

constexpr int kNewFolder = 0x6c100001;
constexpr int kNewFolderForTab = 0x6c100002;
constexpr int kNewWorkspace = 0x6c100003;
constexpr int kMoveToFolder = 0x6c200000;
std::string_view ContextGlyph(int command) {
  switch (command) {
    case kNewFolder:
    case kNewFolderForTab:
      return "\uE8F4";
    case kMoveToFolder:
      return "\uE8DE";
    case kNewWorkspace:
      return "\uE710";
    default:
      break;
  }
  if (command == WorkspaceTabMenuModel::kSubmenu) {
    return "\uE8AB";
  }
  if (auto glyph = SidebarMenuModel::Glyph(command); !glyph.empty()) {
    return glyph;
  }
  return BrowserCommandGlyph(command);
}

std::string_view SiteDataGlyph(int command) {
  if (const auto glyph = SiteDataMenuModel::Glyph(command); !glyph.empty()) {
    return glyph;
  }
  switch (command) {
    case IDC_TAKE_SCREENSHOT:
      return "\uE722";
    case IDC_SHOW_READING_MODE:
      return "\uE8A4";
    case IDC_SHARING_HUB:
      return "\uE72D";
    default:
      return BrowserCommandGlyph(command);
  }
}
std::string_view TabContextGlyph(int command) {
  if (command == WorkspaceTabMenuModel::kEssential) {
    return "\uE734";
  }
  if (const auto glyph = TabCommandGlyph(command); !glyph.empty()) {
    return glyph;
  }
  return ContextGlyph(command);
}
std::string_view MutedTabContextGlyph(int command) {
  if (command == TabStripModel::CommandToggleSiteMuted ||
      command == TabStripModel::CommandToggleTabMuted) {
    return MutedTabCommandGlyph(command);
  }
  return TabContextGlyph(command);
}
}  // namespace
struct NativeMenus::Source final : public ui::SimpleMenuModel::Delegate {
  Source(base::WeakPtr<BrowserWindowInterface> browser,
         tabs::TabInterface* target,
         bool tab_menu,
         bool active_scoped,
         MenuModelSnapshot::PublishImage images,
         base::RepeatingCallback<void(KrImageId)> release_image,
         base::RepeatingClosure invalidated,
         ToggleEssential toggle_essential)
      : browser(std::move(browser)),
        tab(target ? target->GetWeakPtr() : nullptr),
        tab_menu(tab_menu),
        active_scoped(active_scoped),
        toggle_essential(std::move(toggle_essential)),
        snapshot(std::move(images),
                 std::move(invalidated),
                 std::move(release_image)) {}
  ~Source() override {
    snapshot.Clear();
    for (auto it = opened.rbegin(); it != opened.rend(); ++it) {
      if (*it) {
        (*it)->MenuWillClose();
      }
    }
    tab_model = nullptr;
  }

  bool TargetValid() const {
    if (kind == KR_MENU_SECURITY && model && !SiteInfoIsValid(*model))
      return false;
    if (!browser || (page_target && !page_model) || (submenu && !*submenu)) {
      return false;
    }
    if (tab_menu || active_scoped) {
      return tab && tab->GetBrowserWindowInterface() == browser.get() &&
             (!active_scoped ||
              browser->GetActiveTabInterface() == tab.get()) &&
             (kind != KR_MENU_SECURITY ||
              tab->GetContents()->GetLastCommittedURL() == page_url);
    }
    return true;
  }
  ui::MenuModel& Root() {
    if (page_target) {
      return *page_model;
    }
    if (submenu) {
      return **submenu;
    }
    return *model;
  }
  void Open(ui::MenuModel& source) {
    auto weak = source.AsWeakPtr();
    if (std::ranges::none_of(
            opened, [&](const auto& item) { return item.get() == &source; })) {
      source.MenuWillShow();
      if (!weak) {
        return;
      }
      opened.push_back(weak);
    }
    for (size_t index = 0; weak && index < source.GetItemCount(); ++index) {
      const auto type = source.GetTypeAt(index);
      if (type == ui::MenuModel::TYPE_SUBMENU ||
          type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU) {
        if (auto* child = source.GetSubmenuModelAt(index)) {
          Open(*child);
        }
      }
    }
  }
  extensions::ContextMenuMatcher* Extensions(int command) const {
    return tab_model &&
                   extensions::ContextMenuMatcher::IsExtensionsCustomCommandId(
                       command)
               ? tab_model->extension_items()
               : nullptr;
  }
  std::optional<size_t> WorkspaceIndex(int command) const {
    if (tab_workspaces) {
      for (size_t index = 0; index < tab_workspaces->GetItemCount(); ++index) {
        if (tab_workspaces->GetCommandIdAt(index) == command) {
          return index;
        }
      }
    }
    return std::nullopt;
  }
  bool IsCommandIdChecked(int command) const override {
    if (!TargetValid()) {
      return false;
    }
    if (command == WorkspaceTabMenuModel::kEssential) {
      return tab && EssentialForTab(tab->GetContents());
    }
    if (auto* extensions = Extensions(command)) {
      return extensions->IsCommandIdChecked(command);
    }
    return false;
  }
  bool IsCommandIdEnabled(int command) const override {
    if (!TargetValid()) {
      return false;
    }
    auto* view = BrowserView::GetBrowserViewForBrowser(browser.get());
    auto* adapter = view ? view->krypton_ui() : nullptr;
    if (command == kChangeTabIcon) {
      return tab && adapter && adapter->site_customization();
    }

    if (command == kNewFolder || command == kNewFolderForTab ||
        command == kMoveToFolder || folder_targets.contains(command)) {
      return adapter && adapter->CanFolders() &&
             (command != kNewFolderForTab || tab);
    }
    if (command == kNewWorkspace) {
      return adapter && !browser->GetProfile()->IsOffTheRecord();
    }
    if (command == MultiPageMenuModel::kSubmenu) {
      return multi_page_menu && adapter && adapter->MultiPageEnabled();
    }
    if (command == IDC_NEW_SPLIT_TAB ||
        (tab_menu && command == TabStripModel::CommandAddToSplit)) {
      return adapter && adapter->MultiPageEnabled();
    }
    if (command == WorkspaceTabMenuModel::kEssential && adapter &&
        adapter->multi_page() &&
        adapter->multi_page()->Describe(
            base::checked_cast<KrTabId>(tab->GetHandle().raw_value()))) {
      return false;
    }
    if (command == WorkspaceTabMenuModel::kEssential) {
      return tab_menu && tab && toggle_essential &&
             !browser->GetProfile()->IsOffTheRecord();
    }
    if (command == WorkspaceTabMenuModel::kSubmenu) {
      if (tab_workspaces) {
        for (size_t index = 0; index < tab_workspaces->GetItemCount();
             ++index) {
          if (tab_workspaces->IsEnabledAt(index) &&
              tab_workspaces->IsVisibleAt(index)) {
            return true;
          }
        }
      }
      return false;
    }
    if (const auto index = WorkspaceIndex(command)) {
      return tab_workspaces->IsEnabledAt(*index) &&
             tab_workspaces->IsVisibleAt(*index);
    }
    if (tab_menu && command == IDC_BOOKMARK_THIS_TAB) {
      return chrome::CanBookmarkCurrentTab(browser.get());
    }
    if (tab_menu && command == IDC_RESTORE_TAB) {
      return chrome::IsCommandEnabled(browser.get(), command);
    }
    if (auto* extensions = Extensions(command)) {
      return extensions->IsCommandIdEnabled(command);
    }
    if (tab_menu) {
      // These Chromium layouts have no native presentation in this integration.
      // Keep their source items and metadata, but prevent unsupported
      // mutations.
      switch (command) {
        case TabStripModel::CommandToggleGrouped:
        case TabStripModel::CommandAddToNewGroup:
        case TabStripModel::CommandAddToExistingGroup:
        case TabStripModel::CommandAddToNewGroupFromMenuItem:
        case TabStripModel::CommandRemoveFromGroup:
        case TabStripModel::CommandToggleFocusGroup:
        case TabStripModel::CommandAddToSplit:
        case TabStripModel::CommandSwapWithActiveSplit:
        case TabStripModel::CommandArrangeSplit:
        case TabStripModel::CommandToggleVertical:
          return false;
        default:
          break;
      }
      auto* tab_strip_model = browser->GetTabStripModel();
      const auto index = tab_strip_model->GetIndexOfTab(tab.get());
      return index >= 0 &&
             tab_strip_model->IsContextMenuCommandEnabled(
                 index,
                 static_cast<TabStripModel::ContextMenuCommand>(command));
    }
    return chrome::IsCommandEnabled(browser.get(), command);
  }
  bool IsCommandIdVisible(int command) const override {
    if (auto* extensions = Extensions(command)) {
      return extensions->IsCommandIdVisible(command);
    }
    return true;
  }
  bool GetAcceleratorForCommandId(int command,
                                  ui::Accelerator* accelerator) const override {
    if (!browser || !browser->GetWindow()) {
      return false;
    }
    int browser_command = command;
    if (tab_menu && command == TabStripModel::CommandAddToSplit) {
      browser_command = IDC_NEW_SPLIT_TAB;
    } else if (tab_menu && command != IDC_BOOKMARK_THIS_TAB &&
               command != IDC_RESTORE_TAB &&
               !TabStripModel::ContextMenuCommandToBrowserCommand(
                   command, &browser_command)) {
      return false;
    }
    auto* provider = AcceleratorProviderForBrowser(browser.get());
    return provider &&
           provider->GetAcceleratorForCommandId(browser_command, accelerator);
  }
  void ExecuteCommand(int command, int event_flags) override {
    if (command == kNewFolder || command == kNewFolderForTab ||
        folder_targets.contains(command)) {
      if (!IsCommandIdEnabled(command)) {
        return;
      }
      auto* view = BrowserView::GetBrowserViewForBrowser(browser.get());
      auto* adapter = view ? view->krypton_ui() : nullptr;
      if (!adapter) {
        return;
      }
      KrFolderCommand value{};
      value.struct_size = sizeof(value);
      value.kind = folder_targets.contains(command) ? KR_FOLDER_MOVE_TAB
                                                    : KR_FOLDER_CREATE;
      value.folder =
          folder_targets.contains(command) ? folder_targets.at(command) : 0;
      if (tab && command != kNewFolder) {
        value.tab = base::checked_cast<KrTabId>(tab->GetHandle().raw_value());
      }
      std::ignore = adapter->FolderCommand(value);
      return;
    }
    if (command == kNewWorkspace) {
      if (!IsCommandIdEnabled(command)) {
        return;
      }
      auto* view = BrowserView::GetBrowserViewForBrowser(browser.get());
      if (auto* adapter = view ? view->krypton_ui() : nullptr) {
        if (const auto* api = adapter->session().api();
            api && api->show_workspace_editor) {
          std::ignore = api->show_workspace_editor(api->self, 0);
        }
      }
      return;
    }
    if (!IsCommandIdEnabled(command)) {
      return;
    }
    if (command == kChangeTabIcon) {
      auto* view = BrowserView::GetBrowserViewForBrowser(browser.get());
      if (view && view->krypton_ui()) {
        std::ignore = view->krypton_ui()->session().Defer(base::BindOnce(
            [](base::WeakPtr<BrowserWindowInterface> browser,
               base::WeakPtr<tabs::TabInterface> tab) {
              auto* view =
                  browser ? BrowserView::GetBrowserViewForBrowser(browser.get())
                          : nullptr;
              if (view && tab && view->krypton_ui() &&
                  view->krypton_ui()->site_customization()) {
                view->krypton_ui()->site_customization()->EditTabIcon(*tab);
              }
            },
            browser, tab));
      }
      return;
    }
    if (command == IDC_NEW_SPLIT_TAB ||
        (tab_menu && command == TabStripModel::CommandAddToSplit)) {
      auto* view = BrowserView::GetBrowserViewForBrowser(browser.get());
      if (view && view->krypton_ui()) {
        (void)view->krypton_ui()->MultiPageCommand(
            {.struct_size = sizeof(KrMultiPageCommand),
             .kind = KR_MULTI_PAGE_NEW_SPLIT});
      }
      return;
    }
    if (command == WorkspaceTabMenuModel::kSubmenu) {
      return;
    }
    if (command == WorkspaceTabMenuModel::kEssential) {
      toggle_essential.Run(*tab);
      return;
    }
    if (const auto index = WorkspaceIndex(command)) {
      tab_workspaces->ActivatedAt(*index, event_flags);
    } else if (tab_menu && (command == IDC_BOOKMARK_THIS_TAB ||
                            command == IDC_RESTORE_TAB)) {
      if (command == IDC_BOOKMARK_THIS_TAB) {
        // The Chromium bookmark editor belongs to the active page. Activate
        // the explicitly targeted tab before opening its real browser editor.
        auto* tab_strip_model = browser->GetTabStripModel();
        tab_strip_model->ActivateTabAt(
            tab_strip_model->GetIndexOfTab(tab.get()));
      }
      chrome::ExecuteCommand(browser.get(), command);
    } else if (auto* extensions = Extensions(command)) {
      content::ContextMenuParams params;
      params.page_url = tab->GetContents()->GetLastCommittedURL();
      extensions->ExecuteCommand(command, tab->GetContents(), nullptr, params);
    } else if (tab_menu) {
      auto* tab_strip_model = browser->GetTabStripModel();
      tab_strip_model->ExecuteContextMenuCommand(
          tab_strip_model->GetIndexOfTab(tab.get()),
          static_cast<TabStripModel::ContextMenuCommand>(command));
    } else {
      chrome::ExecuteCommandWithDisposition(
          browser.get(), command, ui::DispositionFromEventFlags(event_flags));
    }
  }
  const base::WeakPtr<BrowserWindowInterface> browser;
  const base::WeakPtr<tabs::TabInterface> tab;
  const bool tab_menu;
  const bool active_scoped;
  const ToggleEssential toggle_essential;
  // How the menu was presented, so a structural edit can present it again.
  uint32_t kind = 0;
  GURL page_url;
  MenuModelSnapshot::GlyphTable glyphs = &BrowserCommandGlyph;
  base::span<const int> root_commands;
  std::unique_ptr<ui::MenuModel> tab_workspaces;
  std::unique_ptr<ui::SimpleMenuModel> tab_folders;
  std::map<int, KrFolderId> folder_targets;
  std::unique_ptr<MultiPageMenuModel> multi_page_menu;
  std::unique_ptr<ui::MenuModel> model;
  std::optional<base::WeakPtr<ui::MenuModel>> submenu;
  raw_ptr<TabMenuModel> tab_model = nullptr;
  uint64_t page_target = 0;
  base::WeakPtr<ui::MenuModel> page_model;
  std::vector<base::WeakPtr<ui::MenuModel>> opened;
  MenuModelSnapshot snapshot;
};

NativeMenus::NativeMenus(
    BrowserWindowInterface& browser,
    WindowSession& session,
    MenuModelSnapshot::PublishImage images,
    base::RepeatingCallback<void(KrImageId)> release_image,
    base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(KrSpaceId)>
        workspace_menu,
    base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(KrActionId)>
        extension_menu,
    base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(uint32_t,
                                                           tabs::TabInterface*)>
        context_menu,
    ToggleEssential toggle_essential)
    : browser_(browser.GetWeakPtr()),
      session_(session),
      images_(std::move(images)),
      release_image_(std::move(release_image)),
      workspace_menu_(std::move(workspace_menu)),
      extension_menu_(std::move(extension_menu)),
      context_menu_(std::move(context_menu)),
      toggle_essential_(std::move(toggle_essential)) {}
NativeMenus::~NativeMenus() {
  CloseAll();
}
void NativeMenus::SetExternalMenuFactory(ExternalMenuFactory factory) {
  external_menu_ = std::move(factory);
}

std::optional<int> NativeMenus::SubmenuCommand(actions::ActionId action) {
  switch (action) {
#define KR_SUBMENU_ONE(id) \
  case id:                 \
    return 0;
#define KR_SUBMENU_TWO(id, command) \
  case id:                          \
    return command;
#define KR_SUBMENU_SELECT(_1, _2, macro, ...) macro
#define E(...) \
  KR_SUBMENU_SELECT(__VA_ARGS__, KR_SUBMENU_TWO, KR_SUBMENU_ONE)(__VA_ARGS__)
    SUBMENU_ACTION_IDS
#undef E
#undef KR_SUBMENU_SELECT
#undef KR_SUBMENU_TWO
#undef KR_SUBMENU_ONE
    default:
      return std::nullopt;
  }
}

base::expected<void, std::string> NativeMenus::Present(
    uint32_t request,
    uint32_t kind,
    uint64_t target,
    tabs::TabInterface* tab,
    const ui::ColorProvider* colors) {
  switch (kind) {
    case KR_MENU_DOWNLOAD:
    case KR_MENU_LIBRARY_ITEM:
    case KR_MENU_APP:
    case KR_MENU_CREATE:
    case KR_MENU_LIBRARY:
    case KR_MENU_SPACE:
    case KR_MENU_WORKSPACES:
    case KR_MENU_TAB:
    case KR_MENU_SIDEBAR:
    case KR_MENU_PAGE:
    case KR_MENU_ACTION:
    case KR_MENU_RELOAD:
    case KR_MENU_BACK_HISTORY:
    case KR_MENU_FORWARD_HISTORY:
    case KR_MENU_SITE_DATA:
    case KR_MENU_SECURITY:
      break;
    default:
      return base::unexpected("This menu has no Chromium model.");
  }
  const auto* api = session_->api();
  if (!browser_ || !api) {
    return base::unexpected("The browser window is closed.");
  }
  Closed(request);
  const bool tab_menu = kind == KR_MENU_TAB;
  const bool active_scoped =
      kind == KR_MENU_RELOAD || kind == KR_MENU_PAGE ||
      kind == KR_MENU_SIDEBAR || kind == KR_MENU_SITE_DATA ||
      kind == KR_MENU_SECURITY ||
      (kind == KR_MENU_ACTION &&
       ((target >> 56) == 2 || NativeExtensions::Handles(target))) ||
      kind == KR_MENU_BACK_HISTORY || kind == KR_MENU_FORWARD_HISTORY;
  auto source = std::make_unique<Source>(
      browser_, tab, tab_menu, active_scoped, images_, release_image_,
      base::BindRepeating(&NativeMenus::Invalidated, weak_factory_.GetWeakPtr(),
                          request),
      toggle_essential_);
  if (!source->TargetValid()) {
    return base::unexpected("The menu target is gone.");
  }
  if (tab)
    source->page_url = tab->GetContents()->GetLastCommittedURL();
  MenuModelSnapshot::GlyphTable glyphs = &BrowserCommandGlyph;
  base::span<const int> root_commands;
  // Static: the source keeps these spans for later republishing.
  static constexpr std::array create_commands{
      IDC_NEW_TAB,       IDC_NEW_WINDOW, IDC_NEW_INCOGNITO_WINDOW,
      IDC_NEW_SPLIT_TAB, kNewFolder,     kNewWorkspace};
  static constexpr std::array library_commands{
      IDC_RECENT_TABS_MENU, IDC_SHOW_HISTORY, IDC_BOOKMARKS_MENU,
      IDC_SHOW_BOOKMARK_MANAGER, IDC_SHOW_DOWNLOADS};
  if (kind == KR_MENU_DOWNLOAD || kind == KR_MENU_LIBRARY_ITEM) {
    source->model = external_menu_ ? external_menu_.Run(kind, target) : nullptr;
    if (!source->model) {
      return base::unexpected("The requested item is no longer available.");
    }
  } else if (WorkspaceMenuModel::Handles(kind)) {
    source->model = workspace_menu_.Run(target);
    if (!source->model) {
      return base::unexpected("Workspaces are unavailable in this window.");
    }
    glyphs = &WorkspaceMenuModel::Glyph;
  } else if (kind == KR_MENU_SIDEBAR || kind == KR_MENU_SITE_DATA ||
             kind == KR_MENU_SECURITY) {
    source->model = context_menu_.Run(kind, tab);
    glyphs = kind == KR_MENU_SECURITY    ? &SiteInfoGlyph
             : kind == KR_MENU_SITE_DATA ? &SiteDataGlyph
                                         : &ContextGlyph;
  } else if (kind == KR_MENU_ACTION && NativeExtensions::Handles(target)) {
    source->model = extension_menu_.Run(target);
    if (!source->model) {
      return base::unexpected("The extension action is unavailable.");
    }
  } else if (kind == KR_MENU_PAGE) {
    const auto found = page_menus_.find(target);
    if (found == page_menus_.end() || !found->second) {
      return base::unexpected("The page context menu is closed.");
    }
    source->page_target = target;
    source->page_model = found->second;
    page_menus_.erase(found);
  } else if (tab_menu) {
    auto model = std::make_unique<TabMenuModel>(
        source.get(), TabMenuModelDelegate::From(browser_.get()),
        browser_->GetTabStripModel(),
        browser_->GetTabStripModel()->GetIndexOfTab(tab));
    source->tab_model = model.get();
    const auto layout = ReadLayoutPrefs(*browser_->GetProfile()->GetPrefs());
    if (!(layout.flags & (KR_LAYOUT_TABS_CLASSIC | KR_LAYOUT_TABS_COMPACT |
                          KR_LAYOUT_TABS_DYNAMIC))) {
      // Chromium's Views vertical-tab controller does not own KryptonUI's
      // layout. Select Chromium's directional labels for our saved layout.
      constexpr std::array labels{
          std::pair{TabStripModel::CommandNewTabToRight,
                    IDS_TAB_CXMENU_NEWTABBELOW},
          std::pair{TabStripModel::CommandCloseTabsToLeft,
                    IDS_TAB_CXMENU_CLOSETABSABOVE},
          std::pair{TabStripModel::CommandCloseTabsToRight,
                    IDS_TAB_CXMENU_CLOSETABSBELOW}};
      for (const auto& [command, label] : labels) {
        if (const auto index = model->GetIndexOfCommandId(command)) {
          model->SetLabel(*index, l10n_util::GetStringUTF16(label));
        }
      }
    }
    source->tab_workspaces = context_menu_.Run(kind, tab);
    if (!browser_->GetProfile()->IsOffTheRecord()) {
      source->tab_folders = std::make_unique<ui::SimpleMenuModel>(source.get());
      const auto catalog = ReadFolders(*browser_->GetProfile()->GetPrefs());
      int command = kMoveToFolder + 1;
      source->tab_folders->AddItem(command, u"Outside folders");
      source->folder_targets.emplace(command++, 0);
      for (const auto& folder : catalog.items) {
        std::string label = folder.name;
        const auto* parent = catalog.Find(folder.parent);
        for (unsigned depth = 0; parent && depth < 5; ++depth) {
          label = parent->name + " / " + label;
          parent = catalog.Find(parent->parent);
        }
        auto native_label = base::UTF8ToUTF16(label);
        base::ReplaceSubstringsAfterOffset(&native_label, 0, u"&", u"&&");
        source->tab_folders->AddItem(command, native_label);
        source->folder_targets.emplace(command++, folder.id);
      }
      source->tab_folders->AddSeparator(ui::NORMAL_SEPARATOR);
      source->tab_folders->AddItem(kNewFolderForTab, u"New folder from tab");
      model->AddSubMenu(kMoveToFolder, u"Move to folder",
                        source->tab_folders.get());
    }
    if (source->tab_workspaces && source->tab_workspaces->GetItemCount()) {
      const auto before = model->GetIndexOfCommandId(
          TabStripModel::CommandMoveToExistingWindow);
      model->InsertSubMenuAt(before.value_or(model->GetItemCount()),
                             WorkspaceTabMenuModel::kSubmenu,
                             u"Move to workspace",
                             source->tab_workspaces.get());
    }
    model->AddSeparator(ui::NORMAL_SEPARATOR);
    model->AddItemWithStringId(IDC_BOOKMARK_THIS_TAB, IDS_BOOKMARK_THIS_TAB);
    if (auto* view = BrowserView::GetBrowserViewForBrowser(browser_.get());
        view && view->krypton_ui() && view->krypton_ui()->MultiPageEnabled()) {
      source->multi_page_menu =
          std::make_unique<MultiPageMenuModel>(*browser_.get(), *tab);
      model->AddSubMenu(MultiPageMenuModel::kSubmenu, u"Split view",
                        source->multi_page_menu.get());
    }
    model->AddCheckItem(WorkspaceTabMenuModel::kEssential, u"Essential tab");
    model->AddItem(kChangeTabIcon, u"Change icon\u2026");
    model->AddItemWithStringId(IDC_RESTORE_TAB, IDS_RESTORE_TAB);
    source->model = std::move(model);
    glyphs = tab->GetContents()->IsAudioMuted() ? &MutedTabContextGlyph
                                                : &TabContextGlyph;
  } else if (kind == KR_MENU_BACK_HISTORY || kind == KR_MENU_FORWARD_HISTORY) {
    source->model = std::make_unique<BackForwardMenuModel>(
        browser_.get(), kind == KR_MENU_BACK_HISTORY
                            ? BackForwardMenuModel::ModelType::kBackward
                            : BackForwardMenuModel::ModelType::kForward);
    glyphs = nullptr;
  } else if (kind == KR_MENU_ACTION && (target >> 56) == 1) {
    if ((target >> 56) != 1) {
      return base::unexpected("The toolbar action is gone.");
    }
    source->model = std::make_unique<PinnedActionToolbarButtonMenuModel>(
        browser_.get(), static_cast<actions::ActionId>(target & 0xffffffff));
    glyphs = nullptr;
  } else if (kind == KR_MENU_RELOAD) {
    source->model = ReloadButton::CreateMenuModel(source.get());
  } else if (kind == KR_MENU_CREATE) {
    auto model = std::make_unique<ui::SimpleMenuModel>(source.get());
    model->AddItemWithStringId(IDC_NEW_TAB, IDS_NEW_TAB);
    model->AddItem(kNewFolder, u"New folder");
    model->AddItem(kNewWorkspace, u"New workspace\u2026");
    model->AddItem(IDC_NEW_SPLIT_TAB, u"New split\u2026");
    source->model = std::move(model);
    glyphs = &ContextGlyph;
  } else {
    // These are projections of the real app menu, not independent lists.
    // Missing/policy-hidden source rows stay missing, and source delegates own
    // all labels, metadata, enablement and command execution.
    auto model = std::make_unique<AppMenuModel>(
        AcceleratorProviderForBrowser(browser_.get()), browser_.get());
    model->Init();
    if (auto* view = BrowserView::GetBrowserViewForBrowser(browser_.get());
        view && view->krypton_ui() && view->krypton_ui()->MultiPageEnabled() &&
        !model->GetIndexOfCommandId(IDC_NEW_SPLIT_TAB)) {
      model->InsertItemAt(std::min(size_t{3}, model->GetItemCount()),
                          IDC_NEW_SPLIT_TAB, u"New split\u2026");
    }
    std::vector<ActionMenuModel::Submenu> extras;
    if (kind == KR_MENU_APP) {
      auto* view = BrowserView::GetBrowserViewForBrowser(browser_.get());
      auto* adapter = view ? view->krypton_ui() : nullptr;
      if (adapter && adapter->site_customization()) {
        if (auto workspaces = workspace_menu_.Run(0)) {
          extras.push_back(
              {kWorkspacesMenu, u"Workspaces", std::move(workspaces)});
        }
        extras.push_back({kRoutingMenu, u"Workspace routing",
                          adapter->site_customization()->RoutingMenu()});
        extras.push_back({kBoostMenu, u"Site boosts",
                          adapter->site_customization()->BoostMenu()});
      }
    }
    if (kind == KR_MENU_ACTION) {
      const auto command =
          SubmenuCommand(static_cast<actions::ActionId>(target & 0xffffffff));
      if (!command) {
        return base::unexpected("The toolbar submenu is gone.");
      }
      if (*command) {
        ui::MenuModel* parent = model.get();
        size_t index = 0;
        if (!ui::MenuModel::GetModelAndIndexForCommandId(*command, &parent,
                                                         &index) ||
            !parent->IsEnabledAt(index) || !parent->IsVisibleAt(index)) {
          return base::unexpected("The toolbar submenu is unavailable.");
        }
        auto* child = parent->GetSubmenuModelAt(index);
        if (!child) {
          return base::unexpected("The toolbar action has no submenu model.");
        }
        source->submenu = child->AsWeakPtr();
      }
    }
    source->model = extras.empty()
                        ? std::unique_ptr<ui::MenuModel>(std::move(model))
                        : std::make_unique<ActionMenuModel>(std::move(model),
                                                            std::move(extras));
    if (kind == KR_MENU_CREATE) {
      root_commands = create_commands;
    } else if (kind == KR_MENU_LIBRARY) {
      root_commands = library_commands;
    }
  }
  if (!source->page_target && !source->model) {
    return base::unexpected("The menu is unavailable.");
  }
  source->kind = kind;
  source->glyphs = glyphs;
  source->root_commands = root_commands;
  auto published = Publish(request, *source, colors);
  if (!published.has_value()) {
    return published;
  }
  sources_.emplace(request, std::move(source));
  return {};
}
base::expected<void, std::string> NativeMenus::Publish(
    uint32_t request,
    Source& source,
    const ui::ColorProvider* colors) {
  const auto* api = session_->api();
  if (!api) {
    return base::unexpected("The browser window is closed.");
  }
  auto& root = source.Root();
  source.Open(root);
  auto captured = source.snapshot.Capture(
      root, colors, source.glyphs, source.root_commands,
      source.kind == KR_MENU_SECURITY ? &SiteInfoKeepsOpen : nullptr);
  if (!captured.has_value()) {
    return captured;
  }
  const bool site_data = source.kind == KR_MENU_SITE_DATA;
  return source.snapshot.Publish(
      *api, request, site_data || source.kind == KR_MENU_SECURITY,
      site_data ? static_cast<SiteDataMenuModel&>(root).PanelLayout()
                : base::span<const MenuPanelRow>());
}
void NativeMenus::Republish(uint32_t request) {
  const auto found = sources_.find(request);
  if (found == sources_.end()) {
    return;
  }
  if (!found->second->TargetValid()) {
    Dismiss(request);
    return;
  }
  auto* view = BrowserView::GetBrowserViewForBrowser(browser_.get());
  auto published = Publish(request, *found->second,
                           view ? view->GetColorProvider() : nullptr);
  if (!published.has_value()) {
    LOG(ERROR) << published.error();
    Dismiss(request);
  }
}
base::OnceClosure NativeMenus::CaptureCommand(uint32_t request,
                                              int32_t command) {
  const auto found = sources_.find(request);
  if (found == sources_.end() || !found->second->TargetValid()) {
    return {};
  }
  auto execute = found->second->snapshot.CaptureCommand(request, command);
  if (!execute) {
    return {};
  }
  if (found->second->snapshot.KeepsOpen(command)) {
    return base::BindOnce(
        [](base::WeakPtr<NativeMenus> owner, uint32_t request,
           base::OnceClosure execute) {
          if (!owner)
            return;
          const auto found = owner->sources_.find(request);
          if (found != owner->sources_.end() && found->second->TargetValid())
            std::move(execute).Run();
        },
        weak_factory_.GetWeakPtr(), request, std::move(execute));
  }
  auto source = std::move(found->second);
  sources_.erase(found);
  return base::BindOnce(
      [](std::unique_ptr<Source> source, base::OnceClosure execute) {
        if (source->TargetValid()) {
          std::move(execute).Run();
        }
      },
      std::move(source), std::move(execute));
}
void NativeMenus::Closed(uint32_t request) {
  sources_.erase(request);
}
void NativeMenus::Dismiss(uint32_t request) {
  const auto found = sources_.find(request);
  if (found == sources_.end()) {
    return;
  }
  auto source = std::move(found->second);
  sources_.erase(found);
  if (const auto* api = session_->api()) {
    api->dismiss_menu(api->self, request);
  }
}
void NativeMenus::Invalidated(uint32_t request) {
  // Chromium edits open menus as asynchronous results arrive; Windows spelling
  // removes its unused suggestion placeholders. Follow the rows, as Views
  // menus do, once the model has finished delivering this notification.
  std::ignore = session_->Defer(base::BindOnce(
      &NativeMenus::Republish, weak_factory_.GetWeakPtr(), request));
}
void NativeMenus::CloseAll() {
  weak_factory_.InvalidateWeakPtrs();
  page_menus_.clear();
  while (!sources_.empty()) {
    Dismiss(sources_.begin()->first);
  }
}

base::ScopedClosureRunner NativeMenus::RequestPageMenu(ui::MenuModel& model,
                                                       KrRect anchor) {
  const auto* api = session_->api();
  if (!api || !browser_) {
    return {};
  }
  if (++next_page_menu_ == 0) {
    ++next_page_menu_;
  }
  const auto target = next_page_menu_;
  page_menus_[target] = model.AsWeakPtr();
  auto cancel = base::ScopedClosureRunner(base::BindOnce(
      &NativeMenus::CancelPageMenu, weak_factory_.GetWeakPtr(), target));
  api->request_context_menu(api->self, KR_MENU_PAGE, target, anchor);
  return cancel;
}

void NativeMenus::RefreshPageMenu(const ui::MenuModel& model,
                                  const ui::ColorProvider* colors) {
  const auto* api = session_->api();
  const auto found = std::ranges::find_if(sources_, [&model](const auto& item) {
    return item.second->page_model.get() == &model;
  });
  // A structural edit has already scheduled this menu's republication.
  if (!api || found == sources_.end() || !found->second->snapshot.valid()) {
    return;
  }
  const auto request = found->first;
  // Rows edited in place keep their order, so they keep their command ids.
  auto refreshed = Publish(request, *found->second, colors);
  if (!refreshed.has_value()) {
    LOG(ERROR) << refreshed.error();
    Dismiss(request);
  }
}

void NativeMenus::CancelPageMenu(uint64_t target) {
  page_menus_.erase(target);
  const auto found = std::ranges::find_if(sources_, [target](const auto& item) {
    return item.second->page_target == target;
  });
  if (found != sources_.end()) {
    Dismiss(found->first);
  }
}

NativeContextMenu::NativeContextMenu(base::WeakPtr<BrowserAdapter> owner)
    : owner_(std::move(owner)) {}

NativeContextMenu::~NativeContextMenu() = default;

void NativeContextMenu::Init(ui::SimpleMenuModel* model) {
  model_ = model->AsWeakPtr();
}

void NativeContextMenu::Show(const gfx::Point& screen_point) {
  Cancel();
  if (owner_ && model_) {
    dismissal_ = owner_->RequestPageMenu(*model_, screen_point);
  }
}

void NativeContextMenu::Cancel() {
  dismissal_.RunAndReset();
}

void NativeContextMenu::UpdateMenuItem(int command_id,
                                       bool enabled,
                                       bool hidden,
                                       const std::u16string& title) {
  Refresh();
}

void NativeContextMenu::RebuildMenu() {
  Refresh();
}

void NativeContextMenu::Refresh() {
  // Observers edit rows after the menu opens, as when the link to the selected
  // text is ready or spelling suggestions arrive. Republish the open menu once
  // per turn rather than dismissing it.
  if (!dismissal_ || std::exchange(refresh_pending_, true)) {
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&NativeContextMenu::Republish,
                                weak_factory_.GetWeakPtr()));
}

void NativeContextMenu::Republish() {
  refresh_pending_ = false;
  if (owner_ && model_ && dismissal_) {
    owner_->RefreshPageMenu(*model_);
  }
}

}  // namespace krypton_ui
