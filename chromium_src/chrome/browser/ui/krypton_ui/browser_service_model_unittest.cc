// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_service_model.h"

#include <limits>

#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/user_data_importer/common/importer_data_types.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {

TEST(BrowserServiceModel, EveryBrowserDestinationUsesAnInternalExistingPage) {
  for (uint32_t action = 0; action <= KR_SETTINGS_ACTION_BROWSER_SETTINGS;
       ++action) {
    const auto destination = SettingsDestination(action);
    if (action == KR_SETTINGS_ACTION_MAKE_DEFAULT ||
        action == KR_SETTINGS_ACTION_CHOOSE_DOWNLOAD_FOLDER ||
        action == KR_SETTINGS_ACTION_ADD_PROFILE ||
        action == KR_SETTINGS_ACTION_PROFILE_PICKER) {
      EXPECT_FALSE(destination) << action;
      continue;
    }
    ASSERT_TRUE(destination) << action;
    const GURL url(*destination);
    EXPECT_TRUE(url.is_valid());
    EXPECT_TRUE(url.SchemeIs("krypton"));
    EXPECT_FALSE(url.has_query());
    EXPECT_FALSE(url.has_ref());
    EXPECT_TRUE(url.host() == "settings" || url.host() == "credits" ||
                url.host() == "terms");
  }
  EXPECT_FALSE(SettingsDestination(std::numeric_limits<uint32_t>::max()));
  EXPECT_EQ(SettingsDestination(KR_SETTINGS_ACTION_PRIVACY_SERVICES),
            "krypton://settings/privacy/services");
  EXPECT_EQ(SettingsDestination(KR_SETTINGS_ACTION_MANAGE_STARTUP),
            "krypton://settings/onStartup");
  EXPECT_EQ(SettingsDestination(KR_SETTINGS_ACTION_BROWSER_SETTINGS),
            "krypton://settings/");
}

TEST(BrowserServiceModel,
     ImportMasksTranslateSemanticItemsRatherThanBitPositions) {
  EXPECT_EQ(BrowserImportItems(KR_IMPORT_BOOKMARKS),
            user_data_importer::FAVORITES);
  EXPECT_EQ(BrowserImportItems(KR_IMPORT_HISTORY), user_data_importer::HISTORY);
  EXPECT_EQ(BrowserImportItems(KR_IMPORT_PASSWORDS),
            user_data_importer::PASSWORDS);
  EXPECT_EQ(BrowserImportItems(KR_IMPORT_EXTENSIONS),
            user_data_importer::EXTENSIONS);
  constexpr uint32_t all = KR_IMPORT_BOOKMARKS | KR_IMPORT_HISTORY |
                           KR_IMPORT_PASSWORDS | KR_IMPORT_SEARCH_ENGINES |
                           KR_IMPORT_AUTOFILL | KR_IMPORT_EXTENSIONS;
  for (uint32_t mask = 0; mask <= all; ++mask)
    EXPECT_EQ(NativeImportItems(BrowserImportItems(mask)), mask);
  EXPECT_EQ(BrowserImportItems(1u << 31), 0);
  EXPECT_EQ(NativeImportItems(user_data_importer::COOKIES |
                              user_data_importer::HOME_PAGE),
            0u);
}

TEST(BrowserServiceModel, ClosingFirstRunDoesNotPersistCompletion) {
  EXPECT_TRUE(OnboardingOutcomeCompletesFirstRun(KR_ONBOARDING_COMPLETED));
  EXPECT_TRUE(OnboardingOutcomeCompletesFirstRun(KR_ONBOARDING_SKIPPED));
  EXPECT_FALSE(OnboardingOutcomeCompletesFirstRun(KR_ONBOARDING_CLOSED));
  EXPECT_FALSE(
      OnboardingOutcomeCompletesFirstRun(std::numeric_limits<uint32_t>::max()));
}

TEST(BrowserServiceModel, OlderSettingsTablesKeepTheirExistingSectionContract) {
  uint32_t section = 42;
  KrShellUI api{};
  api.struct_size = offsetof(KrShellUI, show_settings);
  api.self = &section;
  api.show_settings = [](void* self, uint32_t section) noexcept {
    *static_cast<uint32_t*>(self) = section;
    return KR_OK;
  };
  EXPECT_EQ(OpenNativeSettings(nullptr, KR_SETTINGS_CURRENT),
            KR_ERR_UNSUPPORTED);
  EXPECT_EQ(OpenNativeSettings(&api, KR_SETTINGS_CURRENT), KR_ERR_UNSUPPORTED);
  api.struct_size =
      offsetof(KrShellUI, show_settings) + sizeof(api.show_settings);
  EXPECT_EQ(OpenNativeSettings(&api, KR_SETTINGS_CURRENT), KR_OK);
  EXPECT_EQ(section, KR_SETTINGS_GENERAL);
  EXPECT_EQ(OpenNativeSettings(&api, KR_SETTINGS_WORKSPACES),
            KR_ERR_UNSUPPORTED);
  EXPECT_EQ(OpenNativeSettings(&api, KR_SETTINGS_PRIVACY), KR_OK);
  EXPECT_EQ(section, KR_SETTINGS_PRIVACY);
  api.struct_size = sizeof(api);
  api.set_settings_snapshot = [](void*, const KrSettingsSnapshot*) noexcept {
    return KR_OK;
  };
  EXPECT_EQ(OpenNativeSettings(&api, KR_SETTINGS_CURRENT), KR_OK);
  EXPECT_EQ(section, KR_SETTINGS_CURRENT);
  EXPECT_EQ(OpenNativeSettings(&api, KR_SETTINGS_WORKSPACES), KR_OK);
  EXPECT_EQ(section, KR_SETTINGS_WORKSPACES);
}

}  // namespace
}  // namespace krypton_ui
