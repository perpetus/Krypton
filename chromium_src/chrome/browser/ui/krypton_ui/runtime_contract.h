// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_RUNTIME_CONTRACT_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_RUNTIME_CONTRACT_H_

#include <string_view>

#include "krypton_ui/abi.h"

namespace krypton_ui {

struct RuntimeExports {
  KrUiIsSupportedFn is_supported = nullptr;
  KrUiCreateFn create = nullptr;
  KrUiPlatformNameFn platform_name = nullptr;
  KrUiShutdownFn shutdown = nullptr;
};

// An empty view means the contract is complete. Error text has static storage.
[[nodiscard]] std::string_view ValidateExports(const RuntimeExports& exports);
[[nodiscard]] std::string_view ValidateShell(const KrShellUI& shell);

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_RUNTIME_CONTRACT_H_
