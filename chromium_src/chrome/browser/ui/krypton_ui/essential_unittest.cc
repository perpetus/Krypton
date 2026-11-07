// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/essential_prefs.h"

#include <map>
#include <string>

#include "base/values.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {
class EssentialTest : public testing::Test {
 protected:
  EssentialTest() { RegisterEssentialPrefs(prefs.registry()); }
  sync_preferences::TestingPrefServiceSyncable prefs;
};
TEST_F(EssentialTest, PersistsUrlsWithoutDemoEntriesAndSeparatesProfiles) {
  EXPECT_TRUE(ReadEssentials(prefs).items.empty());
  const auto id = SaveEssential(prefs, 0, "https://example.com/");
  ASSERT_TRUE(id);
  EXPECT_EQ(SaveEssential(prefs, *id, "https://example.com/changed"), id);
  const auto saved = ReadEssentials(prefs);
  ASSERT_EQ(saved.items.size(), 1u);
  EXPECT_EQ(saved.items[0].id, *id);
  EXPECT_EQ(saved.items[0].url, "https://example.com/changed");
  sync_preferences::TestingPrefServiceSyncable other;
  RegisterEssentialPrefs(other.registry());
  EXPECT_TRUE(ReadEssentials(other).items.empty());
  EXPECT_TRUE(RemoveEssential(prefs, *id));
  EXPECT_FALSE(SaveEssential(prefs, *id, "https://example.com/old"));
  const auto next = SaveEssential(prefs, 0, "https://example.org/");
  ASSERT_TRUE(next);
  EXPECT_GT(*next, *id);
}
TEST_F(EssentialTest, ReservesCorruptAndDeletedIdsAndRejectsInvalidUrls) {
  base::DictValue data;
  base::ListValue items;
  base::DictValue invalid;
  invalid.Set("id", "40");
  invalid.Set("url", "not a URL");
  items.Append(std::move(invalid));
  data.Set("items", std::move(items));
  prefs.SetDict(kEssentialsPref, std::move(data));
  EXPECT_TRUE(ReadEssentials(prefs).items.empty());
  EXPECT_FALSE(SaveEssential(prefs, 0, "not a URL"));
  EXPECT_EQ(SaveEssential(prefs, 0, "https://example.com/"), 41u);
}
TEST_F(EssentialTest, ExhaustedIdentifiersNeverWrapOrReuseAnEntry) {
  base::DictValue data;
  data.Set("next_id", "0");
  prefs.SetDict(kEssentialsPref, std::move(data));
  EXPECT_FALSE(SaveEssential(prefs, 0, "https://example.com/"));
}
TEST(EssentialSessionTest,
     RoundTripsMembershipAndPreservesOtherBrowserMetadata) {
  std::map<std::string, std::string> data{{"other-service", "untouched"}};
  EncodeWorkspaceSessionData({42, 7}, data);
  EXPECT_EQ(DecodeWorkspaceSessionData(data),
            (WorkspaceTabSessionState{42, 7}));
  EXPECT_EQ(data.at("other-service"), "untouched");
  EncodeWorkspaceSessionData({0, 0}, data);
  EXPECT_FALSE(data.contains(kWorkspaceSessionKey));
  EXPECT_EQ(DecodeWorkspaceSessionData(data), WorkspaceTabSessionState());
  data[kEssentialSessionKey] = "invalid";
  data[kWorkspaceSessionKey] = "-1";
  EXPECT_EQ(DecodeWorkspaceSessionData(data), WorkspaceTabSessionState());
  EXPECT_EQ(data.at("other-service"), "untouched");
}
}  // namespace
}  // namespace krypton_ui
