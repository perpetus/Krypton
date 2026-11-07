// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/native_backdrop.h"

#include "base/command_line.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/canvas.h"
#include "ui/views/view.h"

namespace krypton_ui {
namespace {

class NativeBackdropTest : public testing::Test {
 public:
  static void SetUpTestSuite() { base::CommandLine::Init(0, nullptr); }
};

TEST_F(NativeBackdropTest, PaintsSolidChromeOnEverySideOfThePage) {
  NativeBackdrop backdrop;
  KrWindowBackdrop value{};
  value.struct_size = sizeof(value);
  value.base = 0xff503480;
  ASSERT_TRUE(backdrop.Set(value));

  views::View view;
  view.SetSize({800, 600});
  gfx::Canvas canvas(view.size(), 1, false);
  backdrop.Paint(&canvas, &view);
  const auto bitmap = canvas.GetBitmap();
  EXPECT_EQ(value.base, bitmap.getColor(400, 0));
  EXPECT_EQ(value.base, bitmap.getColor(799, 300));
  EXPECT_EQ(value.base, bitmap.getColor(400, 599));
}

TEST_F(NativeBackdropTest, GlanceThemeAcceptsLegacyAndPartialRecords) {
  NativeBackdrop backdrop;
  KrWindowBackdrop value{};
  value.struct_size = sizeof(value);
  value.pane_outline = 0xff445566;
  value.glance_button = 0xff181818;
  value.glance_button_hover = 0xff292929;
  value.glance_button_icon = 0xff887766;
  value.glance_shadow = 0x3d000000;
  value.glance_button_shadow = 0x12000000;
  ASSERT_TRUE(backdrop.Set(value));
  EXPECT_EQ(backdrop.glance_button(), value.glance_button);
  EXPECT_EQ(backdrop.glance_button_hover(), value.glance_button_hover);
  EXPECT_EQ(backdrop.glance_button_icon(), value.glance_button_icon);
  EXPECT_EQ(backdrop.glance_shadow(), value.glance_shadow);
  EXPECT_EQ(backdrop.glance_button_shadow(), value.glance_button_shadow);
  value.struct_size = offsetof(KrWindowBackdrop, glance_button_hover);
  ASSERT_TRUE(backdrop.Set(value));
  EXPECT_EQ(backdrop.glance_button(), value.glance_button);
  EXPECT_EQ(backdrop.glance_button_hover(), 0u);
  value.struct_size = offsetof(KrWindowBackdrop, glance_button);
  ASSERT_TRUE(backdrop.Set(value));
  EXPECT_EQ(backdrop.pane_outline(), value.pane_outline);
  EXPECT_EQ(backdrop.glance_button(), 0u);
  EXPECT_EQ(backdrop.glance_button_icon(), 0u);
}

TEST_F(NativeBackdropTest, GradientUsesWholeWindowAndUpdatesAfterResize) {
  NativeBackdrop backdrop;
  const KrGradientLayer layer{
      sizeof(layer), KR_GRADIENT_DIAGONAL, 0xff9040c0, 0, 0, 0, 1};
  KrWindowBackdrop value{};
  value.struct_size = sizeof(value);
  value.base = 0xff204060;
  value.layer_count = 1;
  value.layers = &layer;
  value.layer_size = sizeof(layer);
  ASSERT_TRUE(backdrop.Set(value));

  views::View view;
  for (const gfx::Size size : {gfx::Size(800, 600), gfx::Size(600, 800)}) {
    view.SetSize(size);
    gfx::Canvas canvas(size, 1, false);
    backdrop.Paint(&canvas, &view);
    const auto bitmap = canvas.GetBitmap();
    EXPECT_NE(SK_ColorBLACK, bitmap.getColor(size.width() / 2, 0));
    EXPECT_NE(SK_ColorBLACK,
              bitmap.getColor(size.width() - 1, size.height() / 2));
    EXPECT_NE(SK_ColorBLACK,
              bitmap.getColor(size.width() / 2, size.height() - 1));
    EXPECT_NE(bitmap.getColor(0, 0),
              bitmap.getColor(size.width() - 1, size.height() - 1));
  }
}

TEST_F(NativeBackdropTest, PreservesFadeWhoseEndIsBeyondTheWindow) {
  NativeBackdrop backdrop;
  // The shared two-color theme fades from 30% to 120%. At the right edge
  // this layer must still contribute about 22%, rather than being rejected
  // or having its transparent endpoint moved to 100%.
  const KrGradientLayer layer{
      sizeof(layer), KR_GRADIENT_LINEAR, SK_ColorWHITE, 90, 0, 0.3f, 1.2f};
  KrWindowBackdrop value{};
  value.struct_size = sizeof(value);
  value.base = SK_ColorBLACK;
  value.layer_count = 1;
  value.layers = &layer;
  value.layer_size = sizeof(layer);
  ASSERT_TRUE(backdrop.Set(value));

  views::View view;
  view.SetSize({201, 20});
  gfx::Canvas canvas(view.size(), 1, false);
  backdrop.Paint(&canvas, &view);
  const auto bitmap = canvas.GetBitmap();
  const SkColor right = bitmap.getColor(200, 10);
  EXPECT_NEAR(57, SkColorGetR(right), 2);
  EXPECT_NEAR(57, SkColorGetG(right), 2);
  EXPECT_NEAR(57, SkColorGetB(right), 2);
  EXPECT_EQ(SK_AlphaOPAQUE, SkColorGetA(right));
  EXPECT_EQ(SK_ColorWHITE, bitmap.getColor(0, 10));
}

}  // namespace
}  // namespace krypton_ui
