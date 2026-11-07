// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/anchor_geometry.h"

#include <algorithm>
#include <array>
#include <limits>

#include "chrome/browser/ui/krypton_ui/window_geometry.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {

constexpr AnchorKey kAddress{AnchorKind::kElement, KR_ELEMENT_ADDRESS};
constexpr KrRect kBounds{17.25f, 31.5f, 173.75f, 29.25f};

TEST(KryptonUiAnchorGeometryTest, KeysKeepIndependentControlNamespaces) {
  AnchorGeometry anchors;
  for (const auto kind : {KR_ANCHOR_ELEMENT, KR_ANCHOR_ACTION, KR_ANCHOR_TAB}) {
    EXPECT_TRUE(anchors.Update(kind, 7, kBounds, true));
    EXPECT_TRUE(anchors.Get(*AnchorKey::FromAbi(kind, 7)));
  }
  EXPECT_EQ(anchors.size(), 3u);
  EXPECT_FALSE(AnchorKey::FromAbi(std::numeric_limits<uint32_t>::max(), 7));
  EXPECT_FALSE(anchors.Update(3, 7, kBounds, true));
  EXPECT_EQ(anchors.size(), 3u);
}

TEST(KryptonUiAnchorGeometryTest, UnchangedUpdatesReuseCachedGeometry) {
  AnchorGeometry anchors;
  EXPECT_TRUE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, kBounds, true));
  for (int i = 0; i < 2000; ++i) {
    EXPECT_FALSE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, kBounds, true));
  }
  ASSERT_TRUE(anchors.Get(kAddress));
  EXPECT_TRUE(*anchors.Get(kAddress) ==
              gfx::RectF(17.25f, 31.5f, 173.75f, 29.25f));
}

TEST(KryptonUiAnchorGeometryTest, HideAndRemovalCannotLeaveStaleBounds) {
  AnchorGeometry anchors;
  EXPECT_TRUE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, kBounds, true));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_TRUE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id,
                             {nan, nan, nan, nan}, false));
  EXPECT_FALSE(anchors.Get(kAddress));
  EXPECT_FALSE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, kBounds, false));
  EXPECT_EQ(anchors.size(), 0u);
  EXPECT_TRUE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, kBounds, true));
}

TEST(KryptonUiAnchorGeometryTest, EmptyVisibleControlHasNoAnchor) {
  AnchorGeometry anchors;
  EXPECT_TRUE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, kBounds, true));
  EXPECT_TRUE(
      anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, {1, 2, 0, 3}, true));
  EXPECT_FALSE(anchors.Get(kAddress));
  EXPECT_FALSE(
      anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, {1, 2, 3, 0}, true));
}

TEST(KryptonUiAnchorGeometryTest, MalformedGeometryPreservesLastValidUpdate) {
  AnchorGeometry anchors;
  ASSERT_TRUE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, kBounds, true));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const float max = std::numeric_limits<float>::max();
  const std::array<KrRect, 12> invalid{{
      {nan, 2, 3, 4},
      {1, nan, 3, 4},
      {1, 2, nan, 4},
      {1, 2, 3, nan},
      {inf, 2, 3, 4},
      {1, -inf, 3, 4},
      {1, 2, inf, 4},
      {1, 2, 3, inf},
      {1, 2, -3, 4},
      {1, 2, 3, -4},
      {max, 2, max, 4},
      {1, max, 3, max},
  }};
  for (const auto bounds : invalid) {
    EXPECT_FALSE(anchors.Update(KR_ANCHOR_ELEMENT, kAddress.id, bounds, true));
    ASSERT_TRUE(anchors.Get(kAddress));
    EXPECT_TRUE(*anchors.Get(kAddress) ==
                gfx::RectF(17.25f, 31.5f, 173.75f, 29.25f));
  }
}

TEST(KryptonUiAnchorGeometryTest,
     FractionalBoundsTranslateWithoutDpiRescaling) {
  const auto resolved = ResolveAnchorBounds(
      gfx::RectF(17.25f, 31.5f, 173.75f, 29.25f), gfx::RectF(0, 0, 800, 600),
      gfx::PointF(-3.5f, 7.25f));
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(*resolved == gfx::Rect(13, 38, 175, 30)) << resolved->ToString();
  for (const unsigned dpi : {96u, 120u, 144u, 192u}) {
    const auto state = MakeWindowState(1600, 1200, dpi, 0);
    ASSERT_TRUE(state);
    EXPECT_TRUE(ResolveAnchorBounds(
                    gfx::RectF(17.25f, 31.5f, 173.75f, 29.25f),
                    gfx::RectF(0, 0, state->client.width, state->client.height),
                    gfx::PointF(-3.5f, 7.25f)) == resolved);
  }
}

TEST(KryptonUiAnchorGeometryTest, CompactRevealClipsToActualClient) {
  const auto resolved =
      ResolveAnchorBounds(gfx::RectF(-15, 22, 53, 71), gfx::RectF(0, 0, 31, 67),
                          gfx::PointF(9, 13));
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(*resolved == gfx::Rect(9, 35, 31, 45)) << resolved->ToString();
  EXPECT_FALSE(ResolveAnchorBounds(gfx::RectF(90, 22, 53, 71),
                                   gfx::RectF(0, 0, 31, 67), gfx::PointF()));
  EXPECT_FALSE(
      ResolveAnchorBounds(gfx::RectF(1, 2, 3, 4), gfx::RectF(), gfx::PointF()));
}

TEST(KryptonUiAnchorGeometryTest, TranslationAndRtlKeepPhysicalCoordinates) {
  for (const float width : {1.f, 37.f, 311.f, 1027.f}) {
    for (const float height : {1.f, 41.f, 293.f, 809.f}) {
      const gfx::RectF client(0, 0, width, height);
      for (const float x : {-150.f, 0.f, 17.f, width - 23.f, width + 1.f}) {
        const gfx::RectF control(x, 0, 37, 29);
        const auto left = ResolveAnchorBounds(control, client, gfx::PointF());
        const auto shifted =
            ResolveAnchorBounds(control, client, gfx::PointF(-137, 53));
        ASSERT_EQ(left.has_value(), shifted.has_value());
        if (left) {
          auto expected = *left;
          expected.Offset(-137, 53);
          EXPECT_TRUE(*shifted == expected) << shifted->ToString();
          // Coordinates from the native module are already RTL-resolved. The
          // host must not mirror these rectangles a second time.
          EXPECT_EQ(left->x(), std::max(0.f, x));
        }
      }
    }
  }
}

TEST(KryptonUiAnchorGeometryTest, InvalidOriginCannotCreateFallbackAnchor) {
  const float inf = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(ResolveAnchorBounds(
      gfx::RectF(1, 2, 3, 4), gfx::RectF(0, 0, 100, 100), gfx::PointF(inf, 0)));
  EXPECT_FALSE(ResolveAnchorBounds(gfx::RectF(1, 2, 3, 4),
                                   gfx::RectF(0, 0, 100, 100),
                                   gfx::PointF(0, -inf)));
}

TEST(KryptonUiAnchorGeometryTest, RemovedTabsReleaseGeometryAtScale) {
  AnchorGeometry anchors;
  for (uint64_t id = 1; id <= 2000; ++id) {
    EXPECT_TRUE(anchors.Update(KR_ANCHOR_TAB, id, kBounds, true));
  }
  EXPECT_EQ(anchors.size(), 2000u);
  for (uint64_t id = 1; id <= 2000; ++id) {
    EXPECT_TRUE(anchors.Update(KR_ANCHOR_TAB, id, {}, false));
  }
  EXPECT_EQ(anchors.size(), 0u);
}

}  // namespace
}  // namespace krypton_ui
