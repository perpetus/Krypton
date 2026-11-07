// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/folder_prefs.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace krypton_ui {
TEST(FolderPrefsTest, RetainsEmptyFoldersNestingNamesIconsAndCollapseState) {
  sync_preferences::TestingPrefServiceSyncable prefs;
  RegisterFolderPrefs(prefs.registry());
  FolderCatalog catalog;
  catalog.next_id = 3;
  catalog.items = {{1, 1, 0, "Work", "\xF0\x9F\x93\x81", 0, true},
                   {2, 1, 1, "Research", "", 1, false}};
  ASSERT_TRUE(WriteFolders(prefs, catalog));
  const auto restored = ReadFolders(prefs);
  EXPECT_EQ(catalog.items, restored.items);
  EXPECT_EQ(restored.next_id, 3u);
  EXPECT_TRUE(restored.IsDescendant(2, 1));
  EXPECT_FALSE(restored.IsDescendant(1, 2));
}
TEST(FolderPrefsTest, RejectsCyclesCrossWorkspaceAndDeepDescendantsAtomically) {
  sync_preferences::TestingPrefServiceSyncable prefs;
  RegisterFolderPrefs(prefs.registry());
  FolderCatalog catalog;
  catalog.items = {{1, 1, 0, "Root"}, {2, 1, 1, "Child"}};
  ASSERT_TRUE(WriteFolders(prefs, catalog));
  catalog.items[0].parent = 2;
  EXPECT_FALSE(WriteFolders(prefs, catalog));
  EXPECT_EQ(ReadFolders(prefs).items[0].parent, 0u);
  catalog.items[0].parent = 0;
  catalog.items[1].space = 2;
  EXPECT_FALSE(catalog.Valid());
  catalog.items[1].space = 1;
  for (uint64_t id = 3; id <= 5; ++id)
    catalog.items.push_back({id, 1, id - 1, "Nested"});
  EXPECT_TRUE(catalog.Valid());
  catalog.items.push_back({6, 1, 5, "Too deep"});
  EXPECT_FALSE(catalog.Valid());
}
TEST(FolderPrefsTest, SessionMetadataRoundTripsWithoutChangingLegacyRecords) {
  std::map<std::string, std::string> data{{"unrelated", "value"}};
  EncodeWorkspaceSessionData({1, 0, 42}, data);
  EXPECT_EQ(DecodeWorkspaceSessionData(data).folder, 42u);
  EXPECT_EQ(data["unrelated"], "value");
  EncodeWorkspaceSessionData({1, 0, 0}, data);
  EXPECT_FALSE(data.contains(kFolderSessionKey));
  EXPECT_EQ(DecodeWorkspaceSessionData(data).folder, 0u);
  data[kFolderSessionKey] = "invalid";
  EXPECT_EQ(DecodeWorkspaceSessionData(data).folder, 0u);
}
}  // namespace krypton_ui
