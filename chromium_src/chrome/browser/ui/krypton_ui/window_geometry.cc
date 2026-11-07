// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/window_geometry.h"

#include <windows.h>

#include <cmath>

namespace krypton_ui {
namespace {

bool SameRect(KrRect first, KrRect second) {
  return first.x == second.x && first.y == second.y &&
         first.width == second.width && first.height == second.height;
}

bool NonnegativeFinite(float value) {
  return std::isfinite(value) && value >= 0;
}

}  // namespace

std::optional<KrWindowState> MakeWindowState(int pixel_width,
                                             int pixel_height,
                                             unsigned dpi,
                                             uint32_t flags) {
  if (pixel_width < 0 || pixel_height < 0 || dpi == 0) {
    return std::nullopt;
  }
  const auto scale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
  return KrWindowState{sizeof(KrWindowState),
                       flags,
                       scale,
                       {0, 0, pixel_width / scale, pixel_height / scale}};
}

bool SameWindowState(const KrWindowState& first, const KrWindowState& second) {
  return first.flags == second.flags &&
         first.scale_factor == second.scale_factor &&
         SameRect(first.client, second.client);
}

bool WindowGeometry::SetContent(KrRect content, float corner_radius) {
  if (!std::isfinite(content.x) || !std::isfinite(content.y) ||
      !NonnegativeFinite(content.width) || !NonnegativeFinite(content.height) ||
      !NonnegativeFinite(corner_radius) ||
      (SameRect(content, content_) && corner_radius == corner_radius_)) {
    return false;
  }
  content_ = content;
  corner_radius_ = corner_radius;
  return true;
}

bool WindowGeometry::SetMetrics(const KrShellMetrics& metrics) {
  if (!KR_HAS_FIELD(&metrics, KrShellMetrics, minimum_client_height) ||
      !NonnegativeFinite(metrics.minimum_client_width) ||
      !NonnegativeFinite(metrics.minimum_client_height) ||
      (metrics.minimum_client_width == minimum_width_ &&
       metrics.minimum_client_height == minimum_height_)) {
    return false;
  }
  minimum_width_ = metrics.minimum_client_width;
  minimum_height_ = metrics.minimum_client_height;
  return true;
}

}  // namespace krypton_ui
