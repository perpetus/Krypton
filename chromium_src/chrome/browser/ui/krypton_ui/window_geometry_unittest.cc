// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/window_geometry.h"

#include <limits>

#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {

TEST(KryptonUiGeometryTest, ConvertsPhysicalClientPixelsAtFractionalDpi) {
  const auto state = MakeWindowState(1501, 901, 144, KR_WINDOW_ACTIVE);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->struct_size, sizeof(KrWindowState));
  EXPECT_EQ(state->flags, static_cast<uint32_t>(KR_WINDOW_ACTIVE));
  EXPECT_FLOAT_EQ(state->scale_factor, 1.5f);
  EXPECT_FLOAT_EQ(state->client.width, 1501.0f / 1.5f);
  EXPECT_FLOAT_EQ(state->client.height, 901.0f / 1.5f);
  EXPECT_FLOAT_EQ(state->client.x, 0);
  EXPECT_FLOAT_EQ(state->client.y, 0);
}

TEST(KryptonUiGeometryTest, RejectsUnavailableNativeDimensionsAndDpi) {
  EXPECT_FALSE(MakeWindowState(-1, 500, 96, 0));
  EXPECT_FALSE(MakeWindowState(500, -1, 96, 0));
  EXPECT_FALSE(MakeWindowState(500, 500, 0, 0));
  EXPECT_TRUE(MakeWindowState(0, 0, 96, 0));
}

TEST(KryptonUiGeometryTest, IncludesFlagsAndDpiWhenComparingWindowState) {
  const auto first = MakeWindowState(1000, 600, 96, 0);
  ASSERT_TRUE(first);
  EXPECT_TRUE(SameWindowState(*first, *first));
  EXPECT_FALSE(SameWindowState(*first, *MakeWindowState(1000, 600, 96, 1)));
  EXPECT_FALSE(SameWindowState(*first, *MakeWindowState(1000, 600, 144, 0)));
  EXPECT_FALSE(SameWindowState(*first, *MakeWindowState(1001, 600, 96, 0)));
}

TEST(KryptonUiGeometryTest,
     ContentChangesAreIncrementalAndRejectInvalidUpdates) {
  WindowGeometry geometry;
  EXPECT_TRUE(geometry.SetContent({121.5f, 40.25f, 601.75f, 500.5f}, 4.25f));
  EXPECT_FALSE(geometry.SetContent({121.5f, 40.25f, 601.75f, 500.5f}, 4.25f));
  EXPECT_FALSE(geometry.SetContent({0, 0, -1, 2}, 0));
  EXPECT_FALSE(geometry.SetContent({0, 0, 1, 2}, -1));
  EXPECT_FALSE(geometry.SetContent(
      {std::numeric_limits<float>::quiet_NaN(), 0, 1, 2}, 0));
  EXPECT_FLOAT_EQ(geometry.content().x, 121.5f);
  EXPECT_FLOAT_EQ(geometry.corner_radius(), 4.25f);
}

TEST(KryptonUiGeometryTest, MinimumDimensionsComeOnlyFromSizedModuleMetrics) {
  WindowGeometry geometry;
  const KrShellMetrics metrics{sizeof(KrShellMetrics), 450.5f, 290.25f};
  EXPECT_TRUE(geometry.SetMetrics(metrics));
  EXPECT_FALSE(geometry.SetMetrics(metrics));
  auto truncated = metrics;
  truncated.struct_size = offsetof(KrShellMetrics, minimum_client_height);
  EXPECT_FALSE(geometry.SetMetrics(truncated));
  auto invalid = metrics;
  invalid.minimum_client_width = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(geometry.SetMetrics(invalid));
  EXPECT_FLOAT_EQ(geometry.minimum_width(), 450.5f);
  EXPECT_FLOAT_EQ(geometry.minimum_height(), 290.25f);
}

}  // namespace
}  // namespace krypton_ui
