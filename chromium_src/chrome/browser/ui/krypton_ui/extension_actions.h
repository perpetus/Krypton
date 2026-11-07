// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_EXTENSION_ACTIONS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_EXTENSION_ACTIONS_H_
#include "krypton_ui/abi.h"

namespace krypton_ui {
inline constexpr KrActionId kCopyLinkAction = uint64_t{6} << 56;
inline constexpr KrActionId kExtensionMenuAction = uint64_t{3} << 56;
inline constexpr bool IsExtensionAction(KrActionId id) {
  return id == kExtensionMenuAction || (id >> 56) == 4;
}
inline KrAction ExtensionMenuAction(KrStr label) {
  // Site-setting controls remain inside the address field, after extension
  // icons. KR_ACTION_TOOLBAR would put this in the separate top command row.
  static constexpr char glyph[] = "\uEA86";
  return {sizeof(KrAction),
          kExtensionMenuAction,
          KR_ACTION_SITE_SETTING,
          KR_ACTION_VISIBLE | KR_ACTION_ENABLED,
          label,
          label,
          0,
          {glyph, sizeof(glyph) - 1},
          {},
          0,
          -1};
}
}  // namespace krypton_ui
#endif
