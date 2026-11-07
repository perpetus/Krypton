// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_MENU_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_MENU_H_
#include <optional>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/krypton_ui/multi_page_controller.h"
#include "ui/menus/simple_menu_model.h"
class BrowserWindowInterface;
namespace tabs {
class TabInterface;
}
namespace krypton_ui {
// Link navigation remains with Chromium's context menu. This model only
// selects its requested shared topology and rechecks link policy/capacity.
class LinkSplitLayoutMenuModel final : public ui::SimpleMenuModel,
                                       public ui::SimpleMenuModel::Delegate {
 public:
  LinkSplitLayoutMenuModel(base::OnceCallback<void(KrSplitLayout)> execute,
                           base::RepeatingCallback<bool()> enabled);
  ~LinkSplitLayoutMenuModel() override;
  bool IsCommandIdEnabled(int command) const override;
  void ExecuteCommand(int command, int flags) override;

 private:
  base::OnceCallback<void(KrSplitLayout)> execute_;
  base::RepeatingCallback<bool()> enabled_;
};
class MultiPageMenuModel final : public ui::SimpleMenuModel,
                                 public ui::SimpleMenuModel::Delegate {
 public:
  enum Command {
    kSubmenu = 0x6d100000,
    kNewSplit,
    kColumns,
    kRows,
    kGrid,
    kPrevious,
    kNext,
    kDetach,
    kUnsplit,
    kLayoutSubmenu,
    kWideLeft,
    kWideRight,
    kWideCenter,
    kMainLeft,
    kMainRight,
    kMainTop,
    kMainBottom,
  };
  MultiPageMenuModel(BrowserWindowInterface& browser, tabs::TabInterface& tab);
  ~MultiPageMenuModel() override;
  bool IsCommandIdEnabled(int command) const override;
  bool IsCommandIdChecked(int command) const override;
  bool GetAcceleratorForCommandId(int command,
                                  ui::Accelerator* accelerator) const override;
  void ExecuteCommand(int command, int flags) override;

 private:
  base::WeakPtr<BrowserWindowInterface> browser_;
  base::WeakPtr<tabs::TabInterface> tab_;
  std::optional<MultiPageController::GroupInfo> group_;
  ui::SimpleMenuModel layouts_{this};
};
}  // namespace krypton_ui
#endif
