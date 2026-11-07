// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/layout_menus.h"

#include "chrome/grit/generated_resources.h"
#include "krypton_ui/abi.h"

namespace krypton_ui {
namespace {

constexpr uint32_t kHorizontalTabs =
    KR_LAYOUT_TABS_CLASSIC | KR_LAYOUT_TABS_COMPACT | KR_LAYOUT_TABS_DYNAMIC;
constexpr uint32_t kCollapsedSidebar =
    KR_LAYOUT_TOOLBAR_COLLAPSED | KR_LAYOUT_SIDEBAR_ICON_ONLY;
constexpr uint32_t kHideTargets =
    KR_LAYOUT_COMPACT_HIDE_SIDEBAR | KR_LAYOUT_COMPACT_HIDE_TOOLBAR;

// The menu rows follow the preview's layout menus and KryptonUI's appearance
// settings, so either one leaves the flags in the same state.
struct Layout {
  explicit Layout(uint32_t flags)
      : vertical(!(flags & kHorizontalTabs)),
        collapsed(flags & kCollapsedSidebar),
        single((flags & KR_LAYOUT_TOOLBAR_SINGLE) && !collapsed) {
    const bool explicit_targets = flags & KR_LAYOUT_COMPACT_TARGETS_EXPLICIT;
    hide_top = (flags & KR_LAYOUT_COMPACT_HIDE_TOOLBAR) ||
               (!vertical && !explicit_targets &&
                !(flags & KR_LAYOUT_COMPACT_HIDE_SIDEBAR));
    hide_side = (flags & KR_LAYOUT_COMPACT_HIDE_SIDEBAR) ||
                (vertical && !explicit_targets &&
                 !(flags & KR_LAYOUT_COMPACT_HIDE_TOOLBAR));
  }
  const bool vertical;
  const bool collapsed;
  const bool single;
  bool hide_top;
  bool hide_side;
};

}  // namespace

bool IsLayoutCommand(int command) {
  return command >= kLayoutMenu && command <= kLastLayoutCommand;
}

bool IsLayoutCommandChecked(uint32_t flags, int command) {
  const Layout layout(flags);
  switch (command) {
    case kLayoutClassic:
      return flags & KR_LAYOUT_TABS_CLASSIC;
    case kLayoutCompact:
      return flags & KR_LAYOUT_TABS_COMPACT;
    case kLayoutDynamic:
      return flags & KR_LAYOUT_TABS_DYNAMIC;
    case kLayoutSidebar:
      return layout.vertical && layout.single;
    case kLayoutSidebarAndToolbar:
      return layout.vertical && !layout.single && !layout.collapsed;
    case kLayoutCollapsed:
      return layout.vertical && layout.collapsed;
    case kSidebarRight:
      return flags & KR_LAYOUT_SIDEBAR_RIGHT;
    case kCenteredAddress:
      return flags & KR_LAYOUT_CENTERED_ADDRESS;
    case kMinimalAddress:
      return flags & KR_LAYOUT_MINIMAL_ADDRESS;
    case kRoundedFrame:
      return !(flags & KR_LAYOUT_SQUARE_FRAME);
    case kCompactMode:
      return flags & KR_LAYOUT_COMPACT_MODE;
    case kCompactHideSidebar:
      return layout.hide_side && !layout.hide_top;
    case kCompactHideToolbar:
      return layout.hide_top && !layout.hide_side;
    case kCompactHideBoth:
      return layout.hide_side && layout.hide_top;
    case kPinTop:
      return !layout.hide_top;
    case kPinSide:
      return !layout.hide_side;
    default:
      return false;
  }
}

bool IsLayoutCommandEnabled(uint32_t flags, int command) {
  const Layout layout(flags);
  switch (command) {
    case kSidebarRight:
    case kCompactHideSidebar:
    case kCompactHideBoth:
    case kPinSide:
      return layout.vertical;
    case kCenteredAddress:
      return !(flags & KR_LAYOUT_TABS_COMPACT) &&
             !(layout.vertical && layout.single);
    case kPinTop:
      return !(layout.vertical && layout.single);
    default:
      return IsLayoutCommand(command);
  }
}

uint32_t ApplyLayoutCommand(uint32_t flags, int command) {
  switch (command) {
    case kLayoutClassic:
    case kLayoutCompact:
    case kLayoutDynamic:
      flags &= ~kHorizontalTabs;
      flags |= command == kLayoutClassic   ? KR_LAYOUT_TABS_CLASSIC
               : command == kLayoutCompact ? KR_LAYOUT_TABS_COMPACT
                                           : KR_LAYOUT_TABS_DYNAMIC;
      break;
    case kLayoutSidebar:
    case kLayoutSidebarAndToolbar:
    case kLayoutCollapsed:
      flags &=
          ~(kHorizontalTabs | kCollapsedSidebar | KR_LAYOUT_TOOLBAR_SINGLE |
            KR_LAYOUT_SIDEBAR_AUTOHIDE | KR_LAYOUT_SIDEBAR_COLLAPSED);
      if (command == kLayoutSidebar) {
        flags |= KR_LAYOUT_TOOLBAR_SINGLE;
      } else if (command == kLayoutCollapsed) {
        flags |= kCollapsedSidebar;
      }
      break;
    case kSidebarRight:
      flags ^= KR_LAYOUT_SIDEBAR_RIGHT;
      break;
    case kCenteredAddress:
      flags ^= KR_LAYOUT_CENTERED_ADDRESS;
      break;
    case kMinimalAddress:
      flags ^= KR_LAYOUT_MINIMAL_ADDRESS;
      break;
    case kRoundedFrame:
      flags ^= KR_LAYOUT_SQUARE_FRAME;
      break;
    case kCompactMode:
      flags ^= KR_LAYOUT_COMPACT_MODE;
      break;
    case kCompactHideSidebar:
    case kCompactHideToolbar:
    case kCompactHideBoth:
      flags &= ~kHideTargets;
      flags |= KR_LAYOUT_COMPACT_MODE | KR_LAYOUT_COMPACT_TARGETS_EXPLICIT;
      if (command != kCompactHideToolbar) {
        flags |= KR_LAYOUT_COMPACT_HIDE_SIDEBAR;
      }
      if (command != kCompactHideSidebar) {
        flags |= KR_LAYOUT_COMPACT_HIDE_TOOLBAR;
      }
      break;
    case kPinTop:
    case kPinSide:
      // Make the implied target explicit before toggling one of them.
      if (!(flags & KR_LAYOUT_COMPACT_TARGETS_EXPLICIT) &&
          !(flags & kHideTargets)) {
        flags |= (flags & kHorizontalTabs) ? KR_LAYOUT_COMPACT_HIDE_TOOLBAR
                                           : KR_LAYOUT_COMPACT_HIDE_SIDEBAR;
      }
      flags |= KR_LAYOUT_COMPACT_TARGETS_EXPLICIT;
      flags ^= command == kPinTop ? KR_LAYOUT_COMPACT_HIDE_TOOLBAR
                                  : KR_LAYOUT_COMPACT_HIDE_SIDEBAR;
      break;
    default:
      break;
  }
  return flags;
}

LayoutMenus::LayoutMenus(ui::SimpleMenuModel::Delegate* delegate)
    : horizontal_(delegate),
      vertical_(delegate),
      layout_(delegate),
      compact_(delegate) {
  constexpr int kHorizontalGroup = 1;
  constexpr int kVerticalGroup = 2;
  horizontal_.AddRadioItemWithStringId(
      kLayoutClassic, IDS_KRYPTON_LAYOUT_CLASSIC, kHorizontalGroup);
  horizontal_.AddRadioItemWithStringId(
      kLayoutCompact, IDS_KRYPTON_LAYOUT_COMPACT, kHorizontalGroup);
  horizontal_.AddRadioItemWithStringId(
      kLayoutDynamic, IDS_KRYPTON_LAYOUT_DYNAMIC, kHorizontalGroup);

  vertical_.AddRadioItemWithStringId(kLayoutSidebar, IDS_KRYPTON_LAYOUT_SIDEBAR,
                                     kVerticalGroup);
  vertical_.AddRadioItemWithStringId(kLayoutSidebarAndToolbar,
                                     IDS_KRYPTON_LAYOUT_SIDEBAR_AND_TOOLBAR,
                                     kVerticalGroup);
  vertical_.AddRadioItemWithStringId(
      kLayoutCollapsed, IDS_KRYPTON_LAYOUT_COLLAPSED, kVerticalGroup);
  vertical_.AddSeparator(ui::NORMAL_SEPARATOR);
  vertical_.AddCheckItemWithStringId(kSidebarRight,
                                     IDS_KRYPTON_LAYOUT_SIDEBAR_RIGHT);

  layout_.AddSubMenuWithStringId(
      kHorizontalTabsMenu, IDS_KRYPTON_LAYOUT_HORIZONTAL_TABS, &horizontal_);
  layout_.AddSubMenuWithStringId(kVerticalTabsMenu,
                                 IDS_KRYPTON_LAYOUT_VERTICAL_TABS, &vertical_);
  layout_.AddSeparator(ui::NORMAL_SEPARATOR);
  layout_.AddCheckItemWithStringId(kCenteredAddress,
                                   IDS_KRYPTON_LAYOUT_CENTERED_ADDRESS);
  layout_.AddCheckItemWithStringId(kMinimalAddress,
                                   IDS_KRYPTON_LAYOUT_MINIMAL_ADDRESS);
  layout_.AddCheckItemWithStringId(kRoundedFrame,
                                   IDS_KRYPTON_LAYOUT_ROUNDED_FRAME);

  compact_.AddCheckItemWithStringId(kCompactMode, IDS_KRYPTON_COMPACT_MODE);
  compact_.AddSeparator(ui::NORMAL_SEPARATOR);
  compact_.AddCheckItemWithStringId(kCompactHideSidebar,
                                    IDS_KRYPTON_COMPACT_HIDE_SIDEBAR);
  compact_.AddCheckItemWithStringId(kCompactHideToolbar,
                                    IDS_KRYPTON_COMPACT_HIDE_TOOLBAR);
  compact_.AddCheckItemWithStringId(kCompactHideBoth,
                                    IDS_KRYPTON_COMPACT_HIDE_BOTH);
  compact_.AddSubMenuWithStringId(kLayoutMenu, IDS_KRYPTON_LAYOUT_MENU,
                                  &layout_);
  compact_.AddSeparator(ui::NORMAL_SEPARATOR);
  compact_.AddCheckItemWithStringId(kPinTop, IDS_KRYPTON_COMPACT_PIN_TOP);
  compact_.AddCheckItemWithStringId(kPinSide, IDS_KRYPTON_COMPACT_PIN_SIDE);
}

LayoutMenus::~LayoutMenus() = default;

void LayoutMenus::AddTo(ui::SimpleMenuModel& model) {
  model.AddSubMenuWithStringId(kLayoutMenu, IDS_KRYPTON_LAYOUT_MENU, &layout_);
  model.AddSubMenuWithStringId(kCompactMenu, IDS_KRYPTON_COMPACT_MENU,
                               &compact_);
}

}  // namespace krypton_ui
