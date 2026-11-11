// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/native_backdrop.h"

#include "chrome/browser/ui/krypton_ui/frame_material.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

#include "cc/paint/paint_image_builder.h"
#include "cc/paint/paint_shader.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/view.h"

namespace krypton_ui {

NativeBackdrop::NativeBackdrop() = default;
NativeBackdrop::~NativeBackdrop() = default;

bool NativeBackdrop::Set(const KrWindowBackdrop& backdrop) {
  if (!KR_HAS_FIELD(&backdrop, KrWindowBackdrop, grain) ||
      !std::isfinite(backdrop.texture) || backdrop.texture < 0 ||
      backdrop.texture > 1 ||
      (backdrop.layer_count &&
       (!backdrop.layers || backdrop.layer_size < sizeof(KrGradientLayer) ||
        backdrop.layer_count >
            std::numeric_limits<size_t>::max() / backdrop.layer_size))) {
    return false;
  }
  std::vector<KrGradientLayer> layers;
  layers.reserve(backdrop.layer_count);
  for (size_t index = 0; index < backdrop.layer_count; ++index) {
    KrGradientLayer layer;
    std::memcpy(&layer,
                reinterpret_cast<const uint8_t*>(backdrop.layers) +
                    index * backdrop.layer_size,
                sizeof(layer));
    if (!KR_HAS_FIELD(&layer, KrGradientLayer, end) ||
        layer.struct_size > backdrop.layer_size ||
        layer.kind > KR_GRADIENT_DIAGONAL || !std::isfinite(layer.angle) ||
        !std::isfinite(layer.center_x) || !std::isfinite(layer.start) ||
        !std::isfinite(layer.end) || layer.start >= layer.end)
      return false;
    layers.push_back(layer);
  }
  cc::PaintFlags grain;
  if (backdrop.texture > 0) {
    const auto& pixels = backdrop.grain;
    if (!KR_HAS_FIELD(&pixels, KrImage, pixels) || !pixels.pixels ||
        pixels.width <= 0 || pixels.height <= 0 || pixels.stride <= 0 ||
        static_cast<size_t>(pixels.stride) <
            static_cast<size_t>(pixels.width) * 4)
      return false;
    auto image = SkImages::RasterFromPixmapCopy(
        SkPixmap(SkImageInfo::MakeN32Premul(pixels.width, pixels.height),
                 pixels.pixels, static_cast<size_t>(pixels.stride)));
    if (!image)
      return false;
    auto paint_image =
        cc::PaintImageBuilder::WithDefault()
            .set_id(cc::PaintImage::GetNextId())
            .set_image(std::move(image), cc::PaintImage::GetNextContentId())
            .TakePaintImage();
    grain.setShader(cc::PaintShader::MakeImage(paint_image, SkTileMode::kRepeat,
                                               SkTileMode::kRepeat, nullptr));
    grain.setAlphaf(backdrop.texture);
  }
  base_ = backdrop.base;
  pane_outline_ = KR_HAS_FIELD(&backdrop, KrWindowBackdrop, pane_outline)
                      ? backdrop.pane_outline
                      : 0;
  glance_button_ = KR_HAS_FIELD(&backdrop, KrWindowBackdrop, glance_button)
                       ? backdrop.glance_button
                       : 0;
  glance_button_hover_ =
      KR_HAS_FIELD(&backdrop, KrWindowBackdrop, glance_button_hover)
          ? backdrop.glance_button_hover
          : 0;
  glance_button_icon_ =
      KR_HAS_FIELD(&backdrop, KrWindowBackdrop, glance_button_icon)
          ? backdrop.glance_button_icon
          : 0;
  glance_shadow_ = KR_HAS_FIELD(&backdrop, KrWindowBackdrop, glance_shadow)
                       ? backdrop.glance_shadow
                       : 0;
  glance_button_shadow_ =
      KR_HAS_FIELD(&backdrop, KrWindowBackdrop, glance_button_shadow)
          ? backdrop.glance_button_shadow
          : 0;
  transparent_ = KR_HAS_FIELD(&backdrop, KrWindowBackdrop, flags) &&
                 (backdrop.flags & KR_WINDOW_BACKDROP_TRANSPARENT);
  layers_ = std::move(layers);
  grain_ = std::move(grain);
  dirty_ = true;
  return true;
}

void NativeBackdrop::Rebuild(gfx::Size size) const {
  cached_size_ = size;
  dirty_ = false;
  paints_.resize(layers_.size());
  const float width = size.width();
  const float height = size.height();
  for (size_t index = 0; index < layers_.size(); ++index) {
    const auto& layer = layers_[index];
    // CSS fades can end beyond the window (the two-color theme ends at
    // 120%). Skia needs stops in [0, 1], so sample the original fade at the
    // clipped stops, just as the WinUI brushes do. Clamping the positions
    // alone would incorrectly make the layer transparent at the edge.
    const float positions[]{0, std::clamp(layer.start, 0.0f, 1.0f),
                            std::clamp(layer.end, 0.0f, 1.0f), 1};
    std::array<SkColor4f, 4> colors{};
    for (size_t stop = 0; stop < colors.size(); ++stop) {
      colors[stop] = SkColor4f::FromColor(layer.color);
      colors[stop].fA *= 1 - std::clamp((positions[stop] - layer.start) /
                                            (layer.end - layer.start),
                                        0.0f, 1.0f);
    }
    sk_sp<cc::PaintShader> shader;
    if (layer.kind == KR_GRADIENT_RADIAL) {
      const float x = layer.center_x * width;
      const float radius = std::hypot(std::max(x, width - x), height);
      shader = cc::PaintShader::MakeRadialGradient(
          {x, 0}, radius, colors.data(), positions,
          static_cast<int>(colors.size()), SkTileMode::kClamp);
    } else {
      SkPoint points[]{{0, 0}, {width, height}};
      if (layer.kind == KR_GRADIENT_LINEAR) {
        const float radians = layer.angle * std::numbers::pi_v<float> / 180;
        const float x = std::sin(radians);
        const float y = -std::cos(radians);
        const float radius = (std::abs(x) * width + std::abs(y) * height) / 2;
        points[0] = {width / 2 - x * radius, height / 2 - y * radius};
        points[1] = {width / 2 + x * radius, height / 2 + y * radius};
      }
      shader = cc::PaintShader::MakeLinearGradient(
          points, colors.data(), positions, static_cast<int>(colors.size()),
          SkTileMode::kClamp);
    }
    paints_[index].setShader(std::move(shader));
    // Keep quantization contours out of the exposed New Tab Page backdrop.
    // Skia dithers on the raster/GPU path without an animated noise overlay.
    paints_[index].setDither(true);
  }
}

void NativeBackdrop::Paint(gfx::Canvas* canvas, views::View* view) const {
  if (view->size().IsEmpty())
    return;
  // Quality glass: leave the frame unpainted over the DWM system backdrop.
  // Fullscreen windows stay opaque, so keep painting the theme there.
  if (transparent_ &&
      FrameMaterialOf(view->GetWidget()) == FrameMaterial::kTransparent &&
      view->GetWidget()->ShouldWindowContentsBeTransparent()) {
    canvas->DrawColor(SK_ColorTRANSPARENT, SkBlendMode::kSrc);
    return;
  }
  if (dirty_ || cached_size_ != view->size())
    Rebuild(view->size());
  canvas->DrawColor(base_);
  const gfx::RectF bounds(view->GetLocalBounds());
  for (const auto& paint : paints_)
    canvas->DrawRect(bounds, paint);
  if (grain_.getShader())
    canvas->DrawRect(bounds, grain_);
}

}  // namespace krypton_ui
