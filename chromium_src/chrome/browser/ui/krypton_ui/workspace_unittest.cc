// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/workspace_prefs.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/values.h"
#include "chrome/browser/ui/krypton_ui/buildflags.h"
#include "chrome/browser/ui/krypton_ui/workspace_menu.h"
#if BUILDFLAG(ENABLE_KRYPTON_UI)
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#endif
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {
class WorkspaceTest : public testing::Test {
 public:
  WorkspaceTest() { RegisterWorkspacePrefs(prefs.registry()); }
  sync_preferences::TestingPrefServiceSyncable prefs;
};
TEST_F(WorkspaceTest, DefaultWorkspaceExistsWithoutDemoState) {
  const auto catalog = ReadWorkspaces(prefs);
  ASSERT_EQ(catalog.spaces.size(), 1u);
  EXPECT_EQ(catalog.spaces.front().id, kDefaultWorkspace);
  EXPECT_EQ(catalog.spaces.front().name, "Default");
  EXPECT_EQ(LastWorkspace(prefs, catalog), kDefaultWorkspace);
}
TEST_F(WorkspaceTest, CreateRenameAndReloadProfileState) {
  const auto id =
      SaveWorkspace(prefs, {0, " Work & research ", "W", 0xff123456});
  ASSERT_TRUE(id);
  ASSERT_TRUE(SaveWorkspace(prefs, {*id, "Research", "R", 0xff654321}));
  RememberWorkspace(prefs, *id);
  sync_preferences::TestingPrefServiceSyncable reopened;
  RegisterWorkspacePrefs(reopened.registry());
  reopened.Set(kWorkspacesPref,
               prefs.GetUserPrefValue(kWorkspacesPref)->Clone());
  reopened.SetString(kLastWorkspacePref, prefs.GetString(kLastWorkspacePref));
  const auto catalog = ReadWorkspaces(reopened);
  ASSERT_EQ(catalog.spaces.size(), 2u);
  EXPECT_EQ(catalog.spaces[1].id, *id);
  EXPECT_EQ(catalog.spaces[1].name, "Research");
  EXPECT_EQ(catalog.spaces[1].glyph, "R");
  EXPECT_EQ(catalog.spaces[1].color, 0xff654321u);
  EXPECT_EQ(LastWorkspace(reopened, catalog), *id);
}
TEST_F(WorkspaceTest, DeletedIdsAreNeverReusedForRestoredTabs) {
  const auto first = SaveWorkspace(prefs, {0, "First"});
  ASSERT_TRUE(first);
  RememberWorkspace(prefs, *first);
  ASSERT_TRUE(RemoveWorkspace(prefs, *first));
  EXPECT_FALSE(RemoveWorkspace(prefs, kDefaultWorkspace));
  EXPECT_FALSE(RemoveWorkspace(prefs, *first));
  EXPECT_EQ(LastWorkspace(prefs, ReadWorkspaces(prefs)), kDefaultWorkspace);
  const auto next = SaveWorkspace(prefs, {0, "Second"});
  ASSERT_TRUE(next);
  EXPECT_GT(*next, *first);
}
TEST_F(WorkspaceTest, InvalidEditsDoNotChangeProfileState) {
  EXPECT_FALSE(SaveWorkspace(prefs, {0, " \t\n "}));
  EXPECT_FALSE(SaveWorkspace(prefs, {99, "Unknown"}));
  EXPECT_FALSE(SaveWorkspace(prefs, {0, std::string("a\0b", 3)}));
  EXPECT_FALSE(SaveWorkspace(prefs, {0, "Invalid", std::string("\xff", 1)}));
  EXPECT_EQ(ReadWorkspaces(prefs).spaces.size(), 1u);
}
TEST_F(WorkspaceTest, DefaultWorkspaceCanBeRenamedButNotRemoved) {
  ASSERT_TRUE(SaveWorkspace(prefs, {kDefaultWorkspace, "Personal", "P"}));
  EXPECT_FALSE(RemoveWorkspace(prefs, kDefaultWorkspace));
  const auto catalog = ReadWorkspaces(prefs);
  ASSERT_EQ(catalog.spaces.size(), 1u);
  EXPECT_EQ(catalog.spaces.front().name, "Personal");
}
TEST_F(WorkspaceTest, CorruptCatalogFallsBackAndRejectsDuplicateIds) {
  base::ListValue rows;
  rows.Append(
      base::DictValue().Set("id", "2").Set("name", "Work").Set("glyph", "W"));
  rows.Append(base::DictValue()
                  .Set("id", "2")
                  .Set("name", "Duplicate")
                  .Set("glyph", "D"));
  rows.Append(
      base::DictValue().Set("id", "3").Set("name", " ").Set("glyph", ""));
  rows.Append(base::DictValue()
                  .Set("id", "18446744073709551615")
                  .Set("name", "Overflow")
                  .Set("glyph", ""));
  rows.Append("invalid");
  prefs.SetDict(kWorkspacesPref, base::DictValue()
                                     .Set("version", 1)
                                     .Set("next_id", "invalid")
                                     .Set("spaces", std::move(rows)));
  const auto catalog = ReadWorkspaces(prefs);
  ASSERT_EQ(catalog.spaces.size(), 2u);
  EXPECT_EQ(catalog.spaces[1].name, "Work");
  EXPECT_EQ(catalog.next_id, 4u);
  const auto next = SaveWorkspace(prefs, {0, "New workspace"});
  ASSERT_TRUE(next);
  EXPECT_EQ(*next, 4u);
}
TEST_F(WorkspaceTest, LastWorkspaceDoesNotCrossProfiles) {
  const auto id = SaveWorkspace(prefs, {0, "Work"});
  ASSERT_TRUE(id);
  RememberWorkspace(prefs, *id);
  sync_preferences::TestingPrefServiceSyncable other_profile;
  RegisterWorkspacePrefs(other_profile.registry());
  EXPECT_EQ(ReadWorkspaces(other_profile).spaces.size(), 1u);
  EXPECT_EQ(LastWorkspace(other_profile, ReadWorkspaces(other_profile)),
            kDefaultWorkspace);
}
struct Invocation {
  WorkspaceMenuModel::Operation operation;
  KrSpaceId space;
};
class WorkspaceMenuTest : public WorkspaceTest {
 public:
  std::unique_ptr<WorkspaceMenuModel> Menu(bool has_tab = true,
                                           KrSpaceId target = 0) {
    return std::make_unique<WorkspaceMenuModel>(
        ReadWorkspaces(prefs), kDefaultWorkspace, target, has_tab,
        base::BindRepeating(
            [](std::vector<Invocation>* calls,
               WorkspaceMenuModel::Operation operation,
               KrSpaceId space) { calls->push_back({operation, space}); },
            &calls));
  }
  std::vector<Invocation> calls;
};
TEST_F(WorkspaceMenuTest, BothWorkspaceSurfacesUseTheDedicatedModel) {
  EXPECT_TRUE(WorkspaceMenuModel::Handles(KR_MENU_SPACE));
  EXPECT_TRUE(WorkspaceMenuModel::Handles(KR_MENU_WORKSPACES));
  EXPECT_FALSE(WorkspaceMenuModel::Handles(KR_MENU_APP));
  EXPECT_FALSE(WorkspaceMenuModel::Handles(KR_MENU_LIBRARY));
  EXPECT_FALSE(WorkspaceMenuModel::Handles(KR_MENU_SIDEBAR));
}
TEST_F(WorkspaceMenuTest, ShowsSavedWorkspacesAndSwitchesByStableId) {
  const auto id = SaveWorkspace(prefs, {0, "Work & research"});
  ASSERT_TRUE(id);
  auto menu = Menu();
  EXPECT_EQ(menu->GetLabelAt(1), u"Work && research");
  EXPECT_TRUE(menu->IsItemCheckedAt(0));
  EXPECT_FALSE(menu->IsItemCheckedAt(1));
  menu->ActivatedAt(1, 0);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].operation, WorkspaceMenuModel::Operation::kSwitch);
  EXPECT_EQ(calls[0].space, *id);
}
TEST_F(WorkspaceMenuTest, CreationAndEditingUseWorkspaceOperations) {
  const auto id = SaveWorkspace(prefs, {0, "Work"});
  ASSERT_TRUE(id);
  auto menu = Menu(false, *id);
  menu->ActivatedAt(3, 0);
  menu->ActivatedAt(4, 0);
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].operation, WorkspaceMenuModel::Operation::kCreate);
  EXPECT_EQ(calls[0].space, 0u);
  EXPECT_EQ(calls[1].operation, WorkspaceMenuModel::Operation::kEdit);
  EXPECT_EQ(calls[1].space, *id);
}
TEST_F(WorkspaceMenuTest, MoveMenuUsesOnlyOtherRealWorkspaces) {
  const auto id = SaveWorkspace(prefs, {0, "Work"});
  ASSERT_TRUE(id);
  auto menu = Menu();
  auto* move = menu->GetSubmenuModelAt(menu->GetItemCount() - 1);
  ASSERT_NE(move, nullptr);
  ASSERT_EQ(move->GetItemCount(), 1u);
  EXPECT_EQ(move->GetLabelAt(0), u"Work");
  move->ActivatedAt(0, 0);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].operation, WorkspaceMenuModel::Operation::kMoveTab);
  EXPECT_EQ(calls[0].space, *id);
}
TEST_F(WorkspaceMenuTest, NoMoveSubmenuWithoutActiveTabOrDestination) {
  auto menu = Menu();
  EXPECT_EQ(menu->GetItemCount(), 5u);
  ASSERT_TRUE(SaveWorkspace(prefs, {0, "Work"}));
  menu = Menu(false);
  EXPECT_EQ(menu->GetItemCount(), 6u);
  EXPECT_EQ(menu->GetSubmenuModelAt(4), nullptr);
}
TEST_F(WorkspaceMenuTest, ThemeEditorTargetsTheRequestedWorkspace) {
  const auto work = SaveWorkspace(prefs, {0, "Work"});
  ASSERT_TRUE(work);
  auto menu = Menu(false, *work);
  menu->ActivatedAt(5, 0);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].operation, WorkspaceMenuModel::Operation::kTheme);
  EXPECT_EQ(calls[0].space, *work);
}
TEST_F(WorkspaceMenuTest, UnavailableThemeEditorCannotDispatch) {
  int theme_calls = 0;
  WorkspaceMenuModel menu(
      ReadWorkspaces(prefs), kDefaultWorkspace, 0, false,
      base::BindRepeating([](int* count, WorkspaceMenuModel::Operation,
                             KrSpaceId) { ++*count; },
                          &theme_calls),
      false);
  EXPECT_FALSE(menu.IsEnabledAt(4));
  menu.ActivatedAt(4, 0);
  EXPECT_EQ(theme_calls, 0);
}
TEST_F(WorkspaceMenuTest, StaleEditTargetFallsBackAndUnknownCommandsDoNothing) {
  auto menu = Menu(false, 99);
  menu->ActivatedAt(3, 0);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].space, kDefaultWorkspace);
  EXPECT_FALSE(menu->IsCommandIdEnabled(99999));
  menu->ExecuteCommand(99999, 0);
  EXPECT_EQ(calls.size(), 1u);
}
TEST_F(WorkspaceMenuTest, TabMenuUsesTheClickedTabsWorkspaceAndStableIds) {
  const auto work = SaveWorkspace(prefs, {0, "Work & research"});
  const auto other = SaveWorkspace(prefs, {0, "Other"});
  ASSERT_TRUE(work);
  ASSERT_TRUE(other);
  std::vector<KrSpaceId> moved;
  WorkspaceTabMenuModel menu(
      ReadWorkspaces(prefs), *work,
      base::BindRepeating([](KrSpaceId) { return true; }),
      base::BindRepeating([](std::vector<KrSpaceId>* moved,
                             KrSpaceId space) { moved->push_back(space); },
                          &moved));
  ASSERT_EQ(menu.GetItemCount(), 2u);
  EXPECT_EQ(menu.GetLabelAt(0), u"Default");
  EXPECT_EQ(menu.GetLabelAt(1), u"Other");
  EXPECT_TRUE(menu.IsCommandIdEnabled(WorkspaceTabMenuModel::kSubmenu));
  menu.ActivatedAt(1, 0);
  ASSERT_EQ(moved.size(), 1u);
  EXPECT_EQ(moved[0], *other);
}

TEST_F(WorkspaceMenuTest,
       TabMoveRevalidatesDeletedDestinationAndTargetLifetime) {
  const auto work = SaveWorkspace(prefs, {0, "Work & research"});
  ASSERT_TRUE(work);
  bool target_valid = true;
  int moves = 0;
  WorkspaceTabMenuModel menu(
      ReadWorkspaces(prefs), kDefaultWorkspace,
      base::BindRepeating(
          [](PrefService* prefs, bool* valid, KrSpaceId id) {
            return *valid && ReadWorkspaces(*prefs).Contains(id);
          },
          base::Unretained(&prefs), &target_valid),
      base::BindRepeating([](int* moves, KrSpaceId) { ++*moves; }, &moves));
  ASSERT_EQ(menu.GetItemCount(), 1u);
  EXPECT_EQ(menu.GetLabelAt(0), u"Work && research");
  target_valid = false;
  menu.ActivatedAt(0, 0);
  EXPECT_FALSE(menu.IsCommandIdEnabled(WorkspaceTabMenuModel::kSubmenu));
  target_valid = true;
  EXPECT_TRUE(RemoveWorkspace(prefs, *work));
  menu.ActivatedAt(0, 0);
  EXPECT_FALSE(menu.IsCommandIdEnabled(WorkspaceTabMenuModel::kSubmenu));
  EXPECT_EQ(moves, 0);
}

TEST_F(WorkspaceMenuTest, TabMoveDoesNotDispatchRootOrUnknownCommand) {
  int moves = 0;
  WorkspaceTabMenuModel menu(
      ReadWorkspaces(prefs), kDefaultWorkspace,
      base::BindRepeating([](KrSpaceId) { return true; }),
      base::BindRepeating([](int* moves, KrSpaceId) { ++*moves; }, &moves));
  EXPECT_EQ(menu.GetItemCount(), 0u);
  EXPECT_FALSE(menu.IsCommandIdEnabled(WorkspaceTabMenuModel::kSubmenu));
  EXPECT_TRUE(menu.HasCommand(WorkspaceTabMenuModel::kSubmenu));
  EXPECT_FALSE(menu.HasCommand(99999));
  menu.ExecuteCommand(WorkspaceTabMenuModel::kSubmenu, 0);
  menu.ExecuteCommand(99999, 0);
  EXPECT_EQ(moves, 0);
}

#if BUILDFLAG(ENABLE_KRYPTON_UI)
TEST(WorkspaceSessionTest,
     InvalidOrMissingMetadataCannotSelectAnotherWorkspace) {
  EXPECT_EQ(ParseWorkspaceId(""), 0u);
  EXPECT_EQ(ParseWorkspaceId("invalid"), 0u);
  EXPECT_EQ(ParseWorkspaceId("-1"), 0u);
  EXPECT_EQ(ParseWorkspaceId("18446744073709551616"), 0u);
  EXPECT_EQ(ParseWorkspaceId("42"), 42u);
}
#endif
}  // namespace
}  // namespace krypton_ui
