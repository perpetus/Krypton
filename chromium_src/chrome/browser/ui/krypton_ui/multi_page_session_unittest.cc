// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "krypton_ui/abi.h"

#include <limits>

#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
TEST(MultiPageSession, RoundTripsEveryLayoutAndMemberPosition) {
  for (uint32_t count = 2; count <= 4; ++count) {
    for (uint32_t layout = 0; layout <= KR_SPLIT_MAIN_BOTTOM; ++layout) {
      for (uint32_t index = 0; index < count; ++index) {
        const SplitSessionData data{
            91, count, index, layout, {0.3f, 0.4f, 0.6f}};
        EXPECT_EQ(DecodeSplitSessionData(EncodeSplitSessionData(data)), data);
      }
    }
  }
}
TEST(MultiPageSession,
     RejectsMalformedAndIncompleteRecordsWithoutInventingMembers) {
  for (const auto* invalid :
       {"", "1;91;4;0;2;0.5;0.5", "2;91;4;0;2;0.5;0.5;0.5",
        "1;0;4;0;2;0.5;0.5;0.5", "1;91;5;0;2;0.5;0.5;0.5",
        "1;91;4;4;2;0.5;0.5;0.5", "1;91;4;0;10;0.5;0.5;0.5",
        "1;91;4;0;2;nan;0.5;0.5", "1;91;4;0;2;0;0.5;0.5",
        "1;91;4;0;2;0.5;0.5;0.5;extra"})
    EXPECT_FALSE(DecodeSplitSessionData(invalid));
  EXPECT_FALSE(DecodeSplitSessionData(std::string(4096, ';')));
  EXPECT_TRUE(
      EncodeSplitSessionData(
          {91, 4, 0, 2, {std::numeric_limits<float>::infinity(), 0.5f, 0.5f}})
          .empty());
}
}  // namespace krypton_ui
