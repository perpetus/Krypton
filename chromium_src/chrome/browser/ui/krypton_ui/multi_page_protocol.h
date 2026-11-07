// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_PROTOCOL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_PROTOCOL_H_
#include "krypton_ui/abi.h"
namespace krypton_ui {
[[nodiscard]] bool HasCompleteMultiPageProtocol(const KrShellUI& api);
[[nodiscard]] bool HasSnapLayoutProtocol(const KrShellUI& api);
}  // namespace krypton_ui
#endif
