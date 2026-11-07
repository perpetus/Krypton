// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_LAYOUT_MENUS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_LAYOUT_MENUS_H_

#include <cstdint>

#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {

// Layout menu rows. The ids sit below every browser command id and above the
// history menus' entry indices.
enum LayoutCommand : int {
  kLayoutMenu = 32000,
  kHorizontalTabsMenu,
  kVerticalTabsMenu,
  kCompactMenu,
  kLayoutClassic,
  kLayoutCompact,
  kLayoutDynamic,
  kLayoutSidebar,
  kLayoutSidebarAndToolbar,
  kLayoutCollapsed,
  kSidebarRight,
  kCenteredAddress,
  kMinimalAddress,
  kRoundedFrame,
  kCompactMode,
  kCompactHideSidebar,
  kCompactHideToolbar,
  kCompactHideBoth,
  kPinTop,
  kPinSide,
  kLastLayoutCommand = kPinSide,
};

bool IsLayoutCommand(int command);
bool IsLayoutCommandChecked(uint32_t flags, int command);
bool IsLayoutCommandEnabled(uint32_t flags, int command);
uint32_t ApplyLayoutCommand(uint32_t flags, int command);

// The Browser Layout and Compact Mode menus. The delegate answers checked and
// enabled states from the profile's layout flags.
class LayoutMenus {
 public:
  explicit LayoutMenus(ui::SimpleMenuModel::Delegate* delegate);
  LayoutMenus(const LayoutMenus&) = delete;
  LayoutMenus& operator=(const LayoutMenus&) = delete;
  ~LayoutMenus();

  // Adds the two menus as submenus of `model`.
  void AddTo(ui::SimpleMenuModel& model);
  ui::SimpleMenuModel& layout() { return layout_; }
  ui::SimpleMenuModel& compact() { return compact_; }

 private:
  ui::SimpleMenuModel horizontal_;
  ui::SimpleMenuModel vertical_;
  ui::SimpleMenuModel layout_;
  ui::SimpleMenuModel compact_;
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_LAYOUT_MENUS_H_
