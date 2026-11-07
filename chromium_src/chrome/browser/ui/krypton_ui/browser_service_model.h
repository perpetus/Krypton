// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_SERVICE_MODEL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_SERVICE_MODEL_H_

#include <cstdint>
#include <optional>
#include <string_view>

#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"

namespace krypton_ui {

// These destinations are opened through singleton_tabs, bypassing the native
// settings interceptor. No arbitrary preference names or URLs cross the ABI.
std::optional<std::string_view> SettingsDestination(uint32_t action);
KrResult OpenNativeSettings(const KrShellUI* api, uint32_t section);
uint32_t NativeImportItems(uint16_t items);
uint16_t BrowserImportItems(uint32_t items);
bool OnboardingOutcomeCompletesFirstRun(uint32_t outcome);

}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_SERVICE_MODEL_H_
