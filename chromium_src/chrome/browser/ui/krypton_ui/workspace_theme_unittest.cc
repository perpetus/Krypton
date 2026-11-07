// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/workspace_theme_prefs.h"

#include <limits>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {
class WorkspaceThemeTest : public ::testing::Test {
 protected:
  WorkspaceThemeTest() { RegisterWorkspacePrefs(prefs_.registry()); }
  KrWorkspaceTheme Theme(KrSpaceId space = kDefaultWorkspace) {
    auto value = ReadWorkspaceTheme(prefs_, space, KR_THEME_DARK);
    value.flags = KR_THEME_CUSTOMIZED;
    value.harmony = KR_THEME_TRIADIC;
    value.mode = KR_THEME_LIGHTNESS;
    value.point_count = 3;
    value.opacity = 0.625f;
    value.texture = 0.25f;
    value.lightness = 37;
    value.points[0] = {0.25f, 0.5f, 0xff123456};
    value.points[1] = {0.75f, 0.25f, 0xffabcdef};
    value.points[2] = {0.5f, 0.75f, 0xff345678};
    return value;
  }
  sync_preferences::TestingPrefServiceSyncable prefs_;
};
TEST_F(WorkspaceThemeTest, DefaultsAreValidAndUseTheBrowsersScheme) {
  const auto value =
      ReadWorkspaceTheme(prefs_, kDefaultWorkspace, KR_THEME_LIGHT);
  EXPECT_TRUE(ValidWorkspaceTheme(value));
  EXPECT_EQ(value.flags, 0u);
  EXPECT_EQ(value.point_count, 0u);
  EXPECT_EQ(value.scheme, static_cast<uint32_t>(KR_THEME_LIGHT));
}
TEST_F(WorkspaceThemeTest,
       GradientRoundTripsInTheProfileWithoutSavingItsScheme) {
  const auto space = SaveWorkspace(prefs_, {0, "Work"});
  ASSERT_TRUE(space);
  const auto value = Theme(*space);
  ASSERT_TRUE(SaveWorkspaceTheme(prefs_, value));
  sync_preferences::TestingPrefServiceSyncable reopened;
  RegisterWorkspacePrefs(reopened.registry());
  reopened.Set(kWorkspacesPref,
               prefs_.GetUserPrefValue(kWorkspacesPref)->Clone());
  reopened.Set(kWorkspaceThemesPref,
               prefs_.GetUserPrefValue(kWorkspaceThemesPref)->Clone());
  const auto saved = ReadWorkspaceTheme(reopened, *space, KR_THEME_SYSTEM);
  EXPECT_EQ(saved.scheme, static_cast<uint32_t>(KR_THEME_SYSTEM));
  EXPECT_EQ(saved.flags, value.flags);
  EXPECT_EQ(saved.harmony, value.harmony);
  EXPECT_EQ(saved.mode, value.mode);
  EXPECT_EQ(saved.point_count, value.point_count);
  EXPECT_EQ(saved.opacity, value.opacity);
  EXPECT_EQ(saved.texture, value.texture);
  EXPECT_EQ(saved.lightness, value.lightness);
  for (size_t index = 0; index < saved.point_count; ++index) {
    EXPECT_EQ(saved.points[index].x, value.points[index].x);
    EXPECT_EQ(saved.points[index].y, value.points[index].y);
    EXPECT_EQ(saved.points[index].color, value.points[index].color);
  }
  EXPECT_EQ(
      ReadWorkspaceTheme(reopened, kDefaultWorkspace, KR_THEME_SYSTEM).flags,
      0u);
}
TEST_F(WorkspaceThemeTest, DraftsRemainLocalUntilDismissalCommitsThem) {
  WorkspaceThemeState first;
  WorkspaceThemeState second;
  auto draft = Theme();
  ASSERT_TRUE(first.Apply(prefs_, draft, false));
  EXPECT_TRUE(prefs_.GetDict(kWorkspaceThemesPref).empty());
  EXPECT_EQ(first.Resolve(prefs_, 1, KR_THEME_LIGHT).points[0].color,
            draft.points[0].color);
  EXPECT_EQ(second.Resolve(prefs_, 1, KR_THEME_LIGHT).flags, 0u);
  EXPECT_EQ(first.preview_scheme(), draft.scheme);
  ASSERT_TRUE(first.Apply(prefs_, draft, true));
  EXPECT_EQ(first.preview(), nullptr);
  EXPECT_FALSE(first.preview_scheme());
  EXPECT_EQ(second.Resolve(prefs_, 1, KR_THEME_LIGHT).points[0].color,
            draft.points[0].color);
}
TEST_F(WorkspaceThemeTest, AnotherWindowsCommitCannotOverwriteAnOpenDraft) {
  WorkspaceThemeState first;
  WorkspaceThemeState second;
  auto a = Theme();
  auto b = a;
  b.points[0].color = 0xff987654;
  b.scheme = KR_THEME_LIGHT;
  ASSERT_TRUE(first.Apply(prefs_, a, false));
  ASSERT_TRUE(second.Apply(prefs_, b, true));
  EXPECT_EQ(first.Resolve(prefs_, 1, KR_THEME_LIGHT).points[0].color,
            a.points[0].color);
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, 1, KR_THEME_LIGHT).points[0].color,
            b.points[0].color);
  ASSERT_TRUE(first.Apply(prefs_, a, true));
  EXPECT_EQ(second.Resolve(prefs_, 1, KR_THEME_DARK).points[0].color,
            a.points[0].color);
}
TEST_F(WorkspaceThemeTest,
       ProfileIsolationRenameAndResetPreserveOtherWorkspaces) {
  const auto space = SaveWorkspace(prefs_, {0, "Work"});
  ASSERT_TRUE(space);
  ASSERT_TRUE(SaveWorkspaceTheme(prefs_, Theme()));
  ASSERT_TRUE(SaveWorkspaceTheme(prefs_, Theme(*space)));
  ASSERT_TRUE(SaveWorkspace(prefs_, {*space, "Renamed"}));
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, *space, KR_THEME_SYSTEM).point_count,
            3u);
  sync_preferences::TestingPrefServiceSyncable other;
  RegisterWorkspacePrefs(other.registry());
  EXPECT_EQ(ReadWorkspaceTheme(other, 1, KR_THEME_SYSTEM).flags, 0u);
  auto reset = Theme(*space);
  reset.flags = 0;
  EXPECT_TRUE(SaveWorkspaceTheme(prefs_, reset));
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, *space, KR_THEME_SYSTEM).flags, 0u);
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, 1, KR_THEME_SYSTEM).point_count, 3u);
}
TEST_F(WorkspaceThemeTest, DeletedWorkspaceCannotBeRevivedByAStaleEditor) {
  const auto space = SaveWorkspace(prefs_, {0, "Work"});
  ASSERT_TRUE(space);
  auto draft = Theme(*space);
  ASSERT_TRUE(SaveWorkspaceTheme(prefs_, draft));
  WorkspaceThemeState state;
  ASSERT_TRUE(state.Apply(prefs_, draft, false));
  ASSERT_TRUE(RemoveWorkspace(prefs_, *space));
  state.Retain(ReadWorkspaces(prefs_));
  EXPECT_EQ(state.preview(), nullptr);
  EXPECT_FALSE(state.Apply(prefs_, draft, true));
  EXPECT_FALSE(prefs_.GetDict(kWorkspaceThemesPref)
                   .contains(base::NumberToString(*space)));
  const auto replacement = SaveWorkspace(prefs_, {0, "Replacement"});
  ASSERT_TRUE(replacement);
  EXPECT_NE(*replacement, *space);
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, *replacement, KR_THEME_SYSTEM).flags,
            0u);
}
TEST_F(WorkspaceThemeTest, ClosingWindowCanCommitItsLastConfirmedPreview) {
  WorkspaceThemeState state;
  ASSERT_TRUE(state.Apply(prefs_, Theme(), false));
  ASSERT_NE(state.preview(), nullptr);
  const auto draft = *state.preview();
  ASSERT_TRUE(state.Apply(prefs_, draft, true));
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, 1, KR_THEME_SYSTEM).point_count, 3u);
}
TEST_F(WorkspaceThemeTest, InvalidFieldsDoNotReplaceTheSavedGradient) {
  ASSERT_TRUE(SaveWorkspaceTheme(prefs_, Theme()));
  const auto original = prefs_.GetDict(kWorkspaceThemesPref).Clone();
  const auto reject = [&](const KrWorkspaceTheme& invalid) {
    EXPECT_FALSE(ValidWorkspaceTheme(invalid));
    EXPECT_FALSE(SaveWorkspaceTheme(prefs_, invalid));
    EXPECT_EQ(prefs_.GetDict(kWorkspaceThemesPref), original);
  };
  auto invalid = Theme();
  invalid.struct_size = 0;
  reject(invalid);
  invalid = Theme();
  invalid.space = 0;
  reject(invalid);
  invalid = Theme();
  invalid.flags |= 2;
  reject(invalid);
  invalid = Theme();
  invalid.scheme = 3;
  reject(invalid);
  invalid = Theme();
  invalid.harmony = 6;
  reject(invalid);
  invalid = Theme();
  invalid.mode = 3;
  reject(invalid);
  invalid = Theme();
  invalid.point_count = 4;
  reject(invalid);
  invalid = Theme();
  invalid.opacity = 0.2f;
  reject(invalid);
  invalid = Theme();
  invalid.opacity = 0.9f;
  reject(invalid);
  invalid = Theme();
  invalid.texture = 2;
  reject(invalid);
  invalid = Theme();
  invalid.lightness = -1;
  reject(invalid);
  for (float number : {std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity(), -0.1f, 1.1f}) {
    invalid = Theme();
    invalid.points[0].x = number;
    reject(invalid);
    invalid = Theme();
    invalid.points[0].y = number;
    reject(invalid);
  }
  invalid = Theme();
  invalid.opacity = std::numeric_limits<float>::quiet_NaN();
  reject(invalid);
}
TEST_F(WorkspaceThemeTest, MalformedOrFutureStoredRowsFallBackToDefaults) {
  ASSERT_TRUE(SaveWorkspaceTheme(prefs_, Theme()));
  const auto original = prefs_.GetDict(kWorkspaceThemesPref).Clone();
  auto corrupt = original.Clone();
  corrupt.FindDict("1")->Set("version", 99);
  prefs_.SetDict(kWorkspaceThemesPref, std::move(corrupt));
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, 1, KR_THEME_SYSTEM).flags, 0u);
  corrupt = original.Clone();
  corrupt.FindDict("1")->Set("opacity", 100.0);
  prefs_.SetDict(kWorkspaceThemesPref, std::move(corrupt));
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, 1, KR_THEME_SYSTEM).flags, 0u);
  corrupt = original.Clone();
  corrupt.FindDict("1")->FindList("points")->front().GetDict().Set("color",
                                                                   "bad");
  prefs_.SetDict(kWorkspaceThemesPref, std::move(corrupt));
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, 1, KR_THEME_SYSTEM).flags, 0u);
}
TEST_F(WorkspaceThemeTest, InactivePointGarbageIsNotRetainedOrPersisted) {
  auto value = Theme();
  value.point_count = 1;
  value.points[2].x = std::numeric_limits<float>::quiet_NaN();
  WorkspaceThemeState state;
  ASSERT_TRUE(state.Apply(prefs_, value, false));
  EXPECT_EQ(state.preview()->points[2].x, 0);
  ASSERT_TRUE(state.Apply(prefs_, value, true));
  EXPECT_EQ(ReadWorkspaceTheme(prefs_, 1, KR_THEME_DARK).points[2].x, 0);
}
}  // namespace
}  // namespace krypton_ui
