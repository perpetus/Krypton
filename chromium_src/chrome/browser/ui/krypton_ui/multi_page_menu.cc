// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/multi_page_menu.h"

#include <utility>

#include "base/numerics/safe_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/accelerator_utils.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/base/accelerators/accelerator.h"
namespace krypton_ui {
namespace {
BrowserAdapter* Adapter(BrowserWindowInterface* browser) {
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  return view ? view->krypton_ui() : nullptr;
}
KrTabId Identity(const tabs::TabInterface& tab) {
  return base::checked_cast<KrTabId>(tab.GetHandle().raw_value());
}
std::optional<KrSplitLayout> LayoutForCommand(int command) {
  using Menu = MultiPageMenuModel;
  switch (command) {
    case Menu::kColumns:
      return KR_SPLIT_COLUMNS;
    case Menu::kRows:
      return KR_SPLIT_ROWS;
    case Menu::kGrid:
      return KR_SPLIT_GRID;
    case Menu::kWideLeft:
      return KR_SPLIT_WIDE_LEFT;
    case Menu::kWideRight:
      return KR_SPLIT_WIDE_RIGHT;
    case Menu::kWideCenter:
      return KR_SPLIT_WIDE_CENTER;
    case Menu::kMainLeft:
      return KR_SPLIT_MAIN_LEFT;
    case Menu::kMainRight:
      return KR_SPLIT_MAIN_RIGHT;
    case Menu::kMainTop:
      return KR_SPLIT_MAIN_TOP;
    case Menu::kMainBottom:
      return KR_SPLIT_MAIN_BOTTOM;
    default:
      return std::nullopt;
  }
}
}  // namespace
LinkSplitLayoutMenuModel::LinkSplitLayoutMenuModel(
    base::OnceCallback<void(KrSplitLayout)> execute,
    base::RepeatingCallback<bool()> enabled)
    : ui::SimpleMenuModel(this),
      execute_(std::move(execute)),
      enabled_(std::move(enabled)) {
  using Menu = MultiPageMenuModel;
  AddItem(Menu::kColumns, u"Columns");
  AddItem(Menu::kRows, u"Rows");
  AddItem(Menu::kGrid, u"Grid");
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItem(Menu::kWideLeft, u"Wide left");
  AddItem(Menu::kWideRight, u"Wide right");
  AddItem(Menu::kWideCenter, u"Wide center");
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItem(Menu::kMainLeft, u"Main pane left");
  AddItem(Menu::kMainRight, u"Main pane right");
  AddItem(Menu::kMainTop, u"Main pane above");
  AddItem(Menu::kMainBottom, u"Main pane below");
}
LinkSplitLayoutMenuModel::~LinkSplitLayoutMenuModel() = default;
bool LinkSplitLayoutMenuModel::IsCommandIdEnabled(int command) const {
  return execute_ && LayoutForCommand(command) && enabled_.Run();
}
void LinkSplitLayoutMenuModel::ExecuteCommand(int command, int) {
  if (IsCommandIdEnabled(command)) {
    std::move(execute_).Run(*LayoutForCommand(command));
  }
}
MultiPageMenuModel::MultiPageMenuModel(BrowserWindowInterface& browser,
                                       tabs::TabInterface& tab)
    : ui::SimpleMenuModel(this),
      browser_(browser.GetWeakPtr()),
      tab_(tab.GetWeakPtr()) {
  if (auto* adapter = Adapter(&browser); adapter && adapter->multi_page()) {
    group_ = adapter->multi_page()->Describe(Identity(tab));
  }
  AddItem(kNewSplit,
          group_ ? u"Add or choose panes\u2026" : u"New split\u2026");
  AddSeparator(ui::NORMAL_SEPARATOR);
  layouts_.AddCheckItem(kColumns, u"Columns");
  layouts_.AddCheckItem(kRows, u"Rows");
  layouts_.AddCheckItem(kGrid, u"Grid");
  if (auto* adapter = Adapter(&browser);
      adapter && adapter->SnapLayoutsEnabled()) {
    layouts_.AddSeparator(ui::NORMAL_SEPARATOR);
    layouts_.AddCheckItem(kWideLeft, u"Wide left");
    layouts_.AddCheckItem(kWideRight, u"Wide right");
    layouts_.AddCheckItem(kWideCenter, u"Wide center");
    layouts_.AddSeparator(ui::NORMAL_SEPARATOR);
    layouts_.AddCheckItem(kMainLeft, u"Main pane left");
    layouts_.AddCheckItem(kMainRight, u"Main pane right");
    layouts_.AddCheckItem(kMainTop, u"Main pane above");
    layouts_.AddCheckItem(kMainBottom, u"Main pane below");
  }
  auto* adapter = Adapter(&browser);
  const auto* api = adapter ? adapter->session().api() : nullptr;
  if (group_ && api && KR_HAS_SLOT(api, KrShellUI, show_split_options) &&
      api->show_split_options) {
    AddItem(kLayoutSubmenu, u"Snap layouts\u2026");
  } else {
    AddSubMenu(kLayoutSubmenu, u"Snap layouts", &layouts_);
  }
  if (group_) {
    AddSeparator(ui::NORMAL_SEPARATOR);
    AddItem(kPrevious, u"Move pane earlier");
    AddItem(kNext, u"Move pane later");
    AddItem(kDetach, u"Detach pane");
    AddItem(kUnsplit, u"Unsplit");
  }
}
MultiPageMenuModel::~MultiPageMenuModel() = default;
bool MultiPageMenuModel::IsCommandIdEnabled(int command) const {
  auto* adapter = Adapter(browser_.get());
  if (!adapter || !adapter->MultiPageEnabled() || !tab_ ||
      tab_->GetBrowserWindowInterface() != browser_.get()) {
    return false;
  }
  if (command == kNewSplit) {
    return true;
  }
  const auto layout = LayoutForCommand(command);
  if (command == kLayoutSubmenu || layout) {
    if (!group_) {
      return adapter->SnapLayoutsEnabled();
    }
    if (layout && *layout > KR_SPLIT_GRID && !adapter->SnapLayoutsEnabled()) {
      return false;
    }
  }
  if (!group_ || !adapter->multi_page()) {
    return false;
  }
  const auto current = adapter->multi_page()->Describe(Identity(*tab_));
  if (!current || current->id != group_->id ||
      current->revision != group_->revision) {
    return false;
  }
  if (command == kPrevious) {
    return group_->index > 0;
  }
  if (command == kNext) {
    return group_->index + 1 < group_->count;
  }
  return layout.has_value() || command == kLayoutSubmenu ||
         (command >= kPrevious && command <= kUnsplit);
}
bool MultiPageMenuModel::IsCommandIdChecked(int command) const {
  const auto layout = LayoutForCommand(command);
  return group_ && layout && group_->layout == *layout;
}
bool MultiPageMenuModel::GetAcceleratorForCommandId(
    int command,
    ui::Accelerator* accelerator) const {
  if (command != kNewSplit || !browser_ || !browser_->GetWindow()) {
    return false;
  }
  auto* provider = AcceleratorProviderForBrowser(browser_.get());
  return provider &&
         provider->GetAcceleratorForCommandId(IDC_NEW_SPLIT_TAB, accelerator);
}

void MultiPageMenuModel::ExecuteCommand(int command, int) {
  if (!IsCommandIdEnabled(command)) {
    return;
  }
  if (command == kLayoutSubmenu) {
    if (auto* adapter = Adapter(browser_.get())) {
      std::ignore = adapter->ShowSplitOptions(Identity(*tab_));
    }
    return;
  }
  const auto browser = browser_;
  const auto tab = tab_;
  const auto group = group_;
  auto* adapter = Adapter(browser.get());
  const auto layout = LayoutForCommand(command);
  if (command == kNewSplit || (!group && layout)) {
    browser->GetTabStripModel()->ActivateTab(tab.get());
    if (!tab || !tab->IsActivated()) {
      return;
    }
    adapter = Adapter(browser.get());
    if (adapter) {
      (void)adapter->MultiPageCommand(
          {.struct_size = sizeof(KrMultiPageCommand),
           .kind = layout ? KR_MULTI_PAGE_CHOOSE_SPLIT_LAYOUT
                          : KR_MULTI_PAGE_NEW_SPLIT,
           .tab = Identity(*tab),
           .layout = layout.value_or(KR_SPLIT_COLUMNS)});
    }
    return;
  }
  if (!group) {
    return;
  }
  KrSplitEdit edit{
      sizeof(KrSplitEdit), KR_SPLIT_SET_LAYOUT, group->revision, Identity(*tab),
      group->space,        group->index,        group->layout,   0.5f};
  if (layout) {
    edit.layout = *layout;
  } else if (command == kDetach) {
    edit.kind = KR_SPLIT_DETACH;
  } else if (command == kUnsplit) {
    edit.kind = KR_SPLIT_UNSPLIT;
  } else {
    edit.kind = KR_SPLIT_MOVE;
    edit.index = command == kPrevious ? group->index - 1 : group->index + 1;
  }
  (void)adapter->MultiPageCommand({.struct_size = sizeof(KrMultiPageCommand),
                                   .kind = KR_MULTI_PAGE_EDIT_SPLIT,
                                   .tab = Identity(*tab),
                                   .group = group->id,
                                   .revision = group->revision,
                                   .edit = &edit});
}
}  // namespace krypton_ui
