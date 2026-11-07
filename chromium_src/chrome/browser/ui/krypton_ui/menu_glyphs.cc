// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/menu_glyphs.h"

#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/krypton_ui/layout_menus.h"
#include "chrome/browser/ui/krypton_ui/multi_page_commands.h"
#include "chrome/browser/ui/krypton_ui/multi_page_menu.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"

namespace krypton_ui {

std::string_view BrowserCommandGlyph(int command_id) {
  switch (command_id) {
    case IDC_BACK:
      return "\uE72B";
    case IDC_FORWARD:
      return "\uE72A";
    case IDC_CUT:
    case IDC_CONTENT_CONTEXT_CUT:
      return "\uE8C6";
    case IDC_COPY:
    case IDC_CONTENT_CONTEXT_COPY:
    case IDC_CONTENT_CONTEXT_COPYIMAGE:
    case IDC_CONTENT_CONTEXT_COPYLINKTEXT:
      return "\uE8C8";
    case IDC_PASTE:
    case IDC_CONTENT_CONTEXT_PASTE:
    case IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE:
      return "\uE77F";
    case IDC_CONTENT_CONTEXT_SELECTALL:
      return "\uE8B3";
    case IDC_CONTENT_CONTEXT_UNDO:
      return "\uE7A7";
    case IDC_CONTENT_CONTEXT_REDO:
      return "\uE7A6";
    case IDC_CONTENT_CONTEXT_COPYLINKLOCATION:
    case IDC_CONTENT_CONTEXT_COPYIMAGELOCATION:
    case IDC_CONTENT_CONTEXT_COPYLINKTOTEXT:
      return "\uE71B";
    case IDC_CONTENT_CONTEXT_OPENLINKNEWTAB:
    case IDC_CONTENT_CONTEXT_OPENIMAGENEWTAB:
      return "\uE8A5";
    case IDC_CONTENT_CONTEXT_OPENLINKNEWWINDOW:
      return "\uE78B";
    case IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD:
      return "\uE890";
    case IDC_CONTENT_CONTEXT_SAVELINKAS:
    case IDC_CONTENT_CONTEXT_SAVEIMAGEAS:
      return "\uE74E";
    case IDC_CONTENT_CONTEXT_INSPECTELEMENT:
    case IDC_CONTENT_CONTEXT_INSPECTELEMENT_WITH_DEVTOOLS:
      return "\uE943";
    case IDC_NEW_TAB:
      return "\uE8A5";
    case IDC_NEW_WINDOW:
      return "\uE78B";
    case IDC_NEW_INCOGNITO_WINDOW:
      return "\uE890";
    case IDC_NEW_SPLIT_TAB:
    case IDC_CONTENT_CONTEXT_OPENLINKSPLITVIEW:
    case IDC_BOOKMARK_BAR_OPEN_SPLIT_VIEW:
    case MultiPageMenuModel::kSubmenu:
    case MultiPageMenuModel::kNewSplit:
    case MultiPageMenuModel::kLayoutSubmenu:
    case MultiPageMenuModel::kColumns:
      return "\uE89A";
    case MultiPageMenuModel::kRows:
      return "\uE8A9";
    case MultiPageMenuModel::kGrid:
    case MultiPageMenuModel::kWideLeft:
    case MultiPageMenuModel::kWideRight:
    case MultiPageMenuModel::kWideCenter:
    case MultiPageMenuModel::kMainTop:
    case MultiPageMenuModel::kMainBottom:
      return "\uF0E2";
    case MultiPageMenuModel::kMainLeft:
      return "\uE90C";
    case MultiPageMenuModel::kMainRight:
      return "\uE90D";
    case MultiPageMenuModel::kPrevious:
      return "\uE72B";
    case MultiPageMenuModel::kNext:
      return "\uE72A";
    case MultiPageMenuModel::kDetach:
      return "\uE7C3";
    case MultiPageMenuModel::kUnsplit:
      return "\uE89F";
    case kOpenLinkInGlance:
      return "\uE8A1";
    case IDC_RECENT_TABS_MENU:
    case IDC_SHOW_HISTORY:
      return "\uE81C";
    case IDC_CLEAR_BROWSING_DATA:
      return "\uE74D";
    case IDC_RESTORE_TAB:
      return "\uE7A7";
    case IDC_BOOKMARKS_MENU:
    case IDC_SHOW_BOOKMARK_MANAGER:
    case IDC_BOOKMARK_ALL_TABS:
      return "\uE8A4";
    case IDC_BOOKMARK_THIS_TAB:
      return "\uE734";
    case IDC_SHOW_DOWNLOADS:
      return "\uE896";
    case IDC_PASSWORDS_AND_AUTOFILL_MENU:
    case IDC_SHOW_PASSWORD_MANAGER:
      return "\uE8D7";
    case IDC_EXTENSIONS_SUBMENU:
    case IDC_MANAGE_EXTENSIONS:
    case IDC_EXTENSIONS_SUBMENU_MANAGE_EXTENSIONS:
      return "\uEA86";
    case IDC_EXTENSIONS_SUBMENU_VISIT_CHROME_WEB_STORE:
      return "\uE710";
    case IDC_PRINT:
      return "\uE749";
    case IDC_SAVE_PAGE:
      return "\uE74E";
    case IDC_SAVE_AND_SHARE_MENU:
    case IDC_SEND_TAB_TO_SELF:
      return "\uE72D";
    case IDC_SHOW_TRANSLATE:
      return "\uF2B7";
    case IDC_FIND_AND_EDIT_MENU:
    case IDC_FIND:
      return "\uE721";
    case IDC_ZOOM_MINUS:
      return "\uE738";
    case IDC_ZOOM_PLUS:
      return "\uE710";
    case IDC_FULLSCREEN:
      return "\uE740";
    case IDC_OPTIONS:
    case IDC_SHOW_CUSTOMIZE_CHROME_SIDE_PANEL:
      return "\uE713";
    case IDC_MORE_TOOLS_MENU:
      return "\uE712";
    case IDC_HELP_MENU:
    case IDC_HELP_PAGE_VIA_MENU:
      return "\uE897";
    case IDC_FEEDBACK:
      return "\uE90A";
    case IDC_ABOUT:
      return "\uE946";
    case IDC_EXIT:
      return "\uE8BB";
    case IDC_DEV_TOOLS:
    case IDC_VIEW_SOURCE:
      return "\uE943";
    case IDC_TASK_MANAGER:
    case IDC_TASK_MANAGER_APP_MENU:
      return "\uE9D9";
    case IDC_TAKE_SCREENSHOT:
    case IDC_SHARING_HUB_SCREENSHOT:
      return "\uE722";
    case IDC_SHOW_READING_MODE:
      return "\uE8A4";
    case IDC_NAME_WINDOW:
      return "\uE8AC";
    case IDC_RELOAD:
    case IDC_RELOAD_BYPASSING_CACHE:
      return "\uE72C";
    case IDC_RELOAD_CLEARING_CACHE:
      return "\uE74D";
    case kLayoutMenu:
    case kCompactMenu:
      return "\uE8A7";
    case kHorizontalTabsMenu:
    case kLayoutClassic:
    case kLayoutCompact:
    case kLayoutDynamic:
      return "\uE8A9";
    case kVerticalTabsMenu:
    case kLayoutSidebar:
    case kLayoutSidebarAndToolbar:
      return "\uE99C";
    case kLayoutCollapsed:
    case kCompactHideSidebar:
      return "\uE89F";
    case kSidebarRight:
      return "\uE8A0";
    case kCenteredAddress:
      return "\uE8E3";
    case kMinimalAddress:
      return "\uE738";
    case kRoundedFrame:
      return "\uE739";
    case kCompactMode:
      return "\uE73F";
    case kCompactHideToolbar:
      return "\uE921";
    case kCompactHideBoth:
      return "\uE740";
    case kPinTop:
    case kPinSide:
      return "\uE718";
    default:
      return {};
  }
}

std::string_view TabCommandGlyph(int command_id) {
  switch (command_id) {
    case TabStripModel::CommandNewTabToRight:
      return "\uE710";
    case TabStripModel::CommandReload:
      return "\uE72C";
    case TabStripModel::CommandDuplicate:
      return "\uE8C8";
    case TabStripModel::CommandCloseTab:
      return "\uE8BB";
    case TabStripModel::CommandTogglePinned:
      return "\uE718";
    case TabStripModel::CommandToggleSiteMuted:
    case TabStripModel::CommandToggleTabMuted:
      return "\uE74F";
    case TabStripModel::CommandSendTabToSelf:
      return "\uE72D";
    case TabStripModel::CommandMoveTabsToNewWindow:
      return "\uE78B";
    case TabStripModel::CommandGoBack:
      return "\uE72B";
    case TabStripModel::CommandDiscard:
      return "\uE74D";
    default:
      return {};
  }
}

std::string_view MutedTabCommandGlyph(int command_id) {
  return command_id == TabStripModel::CommandToggleSiteMuted ||
                 command_id == TabStripModel::CommandToggleTabMuted
             ? "\uE767"
             : TabCommandGlyph(command_id);
}

}  // namespace krypton_ui
