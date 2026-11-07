// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WINDOW_GEOMETRY_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WINDOW_GEOMETRY_H_

#include <optional>

#include "krypton_ui/abi.h"

namespace krypton_ui {

// Physical HWND client dimensions become module DIPs exactly once at the
// host boundary. These functions contain no browser layout or design values.
[[nodiscard]] std::optional<KrWindowState> MakeWindowState(int pixel_width,
                                                           int pixel_height,
                                                           unsigned dpi,
                                                           uint32_t flags);
[[nodiscard]] bool SameWindowState(const KrWindowState& first,
                                   const KrWindowState& second);

// Geometry is reported synchronously by the module. Invalid updates leave the
// previous state intact; the host must not substitute guessed dimensions.
class WindowGeometry {
 public:
  [[nodiscard]] bool SetContent(KrRect content, float corner_radius);
  [[nodiscard]] bool SetMetrics(const KrShellMetrics& metrics);
  [[nodiscard]] KrRect content() const { return content_; }
  [[nodiscard]] float corner_radius() const { return corner_radius_; }
  [[nodiscard]] float minimum_width() const { return minimum_width_; }
  [[nodiscard]] float minimum_height() const { return minimum_height_; }

 private:
  KrRect content_{};
  float corner_radius_ = 0;
  float minimum_width_ = 0;
  float minimum_height_ = 0;
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_WINDOW_GEOMETRY_H_
