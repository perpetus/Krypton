// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/content_regions.h"

#include <array>

#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {

TEST(KryptonUiContentRegionsTest, PreservesTheExactModuleAreaWithoutServices) {
  const gfx::Rect area(137, 29, 917, 633);
  const auto regions = PartitionContent(area, 0, false, 0, 0);
  EXPECT_TRUE(regions.page == area) << regions.page.ToString();
  EXPECT_TRUE(regions.side_panel.IsEmpty());
  EXPECT_TRUE(regions.bookmarks.IsEmpty());
  EXPECT_TRUE(regions.infobars.IsEmpty());
}

TEST(KryptonUiContentRegionsTest,
     MeasuredServicesTileNarrowAndTranslatedAreas) {
  // Vary measured sizes and origins, including values larger than the client
  // area. Every non-empty region must stay inside it and never overlap a peer.
  for (const int width : {0, 1, 37, 311, 1027}) {
    for (const int height : {0, 1, 41, 293, 809}) {
      for (const int panel : {-1, 0, 17, 457, 2001}) {
        for (const int surface : {-1, 0, 29, 357, 2001}) {
          for (const bool right : {false, true}) {
            const gfx::Rect area(137, 53, width, height);
            const auto regions =
                PartitionContent(area, panel, right, surface, surface);
            const std::array parts{regions.side_panel, regions.bookmarks,
                                   regions.infobars, regions.page};
            int64_t total_area = 0;
            for (size_t i = 0; i < parts.size(); ++i) {
              EXPECT_GE(parts[i].width(), 0);
              EXPECT_GE(parts[i].height(), 0);
              if (!parts[i].IsEmpty()) {
                EXPECT_TRUE(area.Contains(parts[i]));
              }
              total_area +=
                  static_cast<int64_t>(parts[i].width()) * parts[i].height();
              for (size_t j = i + 1; j < parts.size(); ++j) {
                EXPECT_FALSE(parts[i].Intersects(parts[j]));
              }
            }
            EXPECT_EQ(total_area, static_cast<int64_t>(width) * height);
          }
        }
      }
    }
  }
}

TEST(KryptonUiContentRegionsTest, PanelSideDoesNotChangeServiceOrPageSizes) {
  const gfx::Rect area(119, 43, 937, 691);
  const auto left = PartitionContent(area, 317, false, 39, 61);
  const auto right = PartitionContent(area, 317, true, 39, 61);
  EXPECT_TRUE(left.page.size() == right.page.size());
  EXPECT_TRUE(left.bookmarks.size() == right.bookmarks.size());
  EXPECT_TRUE(left.infobars.size() == right.infobars.size());
  EXPECT_EQ(left.page.right(), area.right());
  EXPECT_EQ(right.page.x(), area.x());
  EXPECT_EQ(left.side_panel.x(), area.x());
  EXPECT_EQ(right.side_panel.right(), area.right());
}

}  // namespace
}  // namespace krypton_ui
