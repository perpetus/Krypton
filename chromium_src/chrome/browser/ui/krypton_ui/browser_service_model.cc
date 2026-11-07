// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_service_model.h"

#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/user_data_importer/common/importer_data_types.h"

namespace krypton_ui {
namespace {
constexpr struct {
  uint32_t native;
  uint16_t browser;
} kImportItems[] = {
    {KR_IMPORT_BOOKMARKS, user_data_importer::FAVORITES},
    {KR_IMPORT_HISTORY, user_data_importer::HISTORY},
    {KR_IMPORT_PASSWORDS, user_data_importer::PASSWORDS},
    {KR_IMPORT_SEARCH_ENGINES, user_data_importer::SEARCH_ENGINES},
    {KR_IMPORT_AUTOFILL, user_data_importer::AUTOFILL_FORM_DATA},
    {KR_IMPORT_EXTENSIONS, user_data_importer::EXTENSIONS},
};
}  // namespace

std::optional<std::string_view> SettingsDestination(uint32_t action) {
  switch (action) {
    case KR_SETTINGS_ACTION_MANAGE_STARTUP:
      return "krypton://settings/onStartup";
    case KR_SETTINGS_ACTION_IMPORT:
      return "krypton://settings/importData";
    case KR_SETTINGS_ACTION_PERFORMANCE:
      return "krypton://settings/performance";
    case KR_SETTINGS_ACTION_SYSTEM:
      return "krypton://settings/system";
    case KR_SETTINGS_ACTION_RESET:
      return "krypton://settings/reset";
    case KR_SETTINGS_ACTION_MANAGE_PROFILE:
      return "krypton://settings/manageProfile";
    case KR_SETTINGS_ACTION_FONTS:
      return "krypton://settings/fonts";
    case KR_SETTINGS_ACTION_CLEAR_DATA:
      return "krypton://settings/clearBrowserData";
    case KR_SETTINGS_ACTION_COOKIES:
      return "krypton://settings/cookies";
    case KR_SETTINGS_ACTION_SECURITY:
      return "krypton://settings/security";
    case KR_SETTINGS_ACTION_PRIVACY_SERVICES:
      return "krypton://settings/privacy/services";
    case KR_SETTINGS_ACTION_MANAGE_SEARCH:
      return "krypton://settings/searchEngines";
    case KR_SETTINGS_ACTION_MORE_DOWNLOADS:
      return "krypton://settings/downloads";
    case KR_SETTINGS_ACTION_LANGUAGES:
      return "krypton://settings/languages";
    case KR_SETTINGS_ACTION_ACCESSIBILITY:
      return "krypton://settings/accessibility";
    case KR_SETTINGS_ACTION_ABOUT_BROWSER:
      return "krypton://settings/help";
    case KR_SETTINGS_ACTION_CREDITS:
      return "krypton://credits/";
    case KR_SETTINGS_ACTION_LICENSE:
      return "krypton://terms/";
    case KR_SETTINGS_ACTION_BROWSER_SETTINGS:
      return "krypton://settings/";
    default:
      return std::nullopt;
  }
}

uint32_t NativeImportItems(uint16_t items) {
  uint32_t result = 0;
  for (const auto& item : kImportItems) {
    if (items & item.browser)
      result |= item.native;
  }
  return result;
}

KrResult OpenNativeSettings(const KrShellUI* api, uint32_t section) {
  if (!api || !KR_HAS_SLOT(api, KrShellUI, show_settings) ||
      !api->show_settings)
    return KR_ERR_UNSUPPORTED;
  const bool extended = KR_HAS_SLOT(api, KrShellUI, set_settings_snapshot) &&
                        api->set_settings_snapshot;
  if (!extended) {
    if (section == KR_SETTINGS_CURRENT)
      section = KR_SETTINGS_GENERAL;
    if (section > KR_SETTINGS_ABOUT)
      return KR_ERR_UNSUPPORTED;
  }
  return api->show_settings(api->self, section);
}

uint16_t BrowserImportItems(uint32_t items) {
  uint16_t result = 0;
  for (const auto& item : kImportItems) {
    if (items & item.native)
      result |= item.browser;
  }
  return result;
}

bool OnboardingOutcomeCompletesFirstRun(uint32_t outcome) {
  return outcome == KR_ONBOARDING_COMPLETED || outcome == KR_ONBOARDING_SKIPPED;
}

}  // namespace krypton_ui
