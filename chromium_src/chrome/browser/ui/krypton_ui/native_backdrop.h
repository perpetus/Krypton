// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_BACKDROP_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_BACKDROP_H_

#include <vector>

#include "cc/paint/paint_flags.h"
#include "krypton_ui/abi.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/background.h"

namespace krypton_ui {

// Paints only data resolved by the module, in the same client-DIP coordinates
// as its islands. No design tokens or private module headers cross the seam.
class NativeBackdrop final : public views::Background {
 public:
  NativeBackdrop();
  ~NativeBackdrop() override;
  [[nodiscard]] bool Set(const KrWindowBackdrop& backdrop);
  void Paint(gfx::Canvas* canvas, views::View* view) const override;
  // ARGB outline of a split's focused page; transparent draws none.
  [[nodiscard]] KrColor pane_outline() const { return pane_outline_; }
  // The frame shows the platform's system backdrop instead of the theme.
  [[nodiscard]] bool transparent() const { return transparent_; }
  [[nodiscard]] KrColor base() const { return base_; }

  [[nodiscard]] KrColor glance_button() const { return glance_button_; }
  [[nodiscard]] KrColor glance_button_hover() const {
    return glance_button_hover_;
  }
  [[nodiscard]] KrColor glance_button_icon() const {
    return glance_button_icon_;
  }
  [[nodiscard]] KrColor glance_shadow() const { return glance_shadow_; }
  [[nodiscard]] KrColor glance_button_shadow() const {
    return glance_button_shadow_;
  }

 private:
  void Rebuild(gfx::Size size) const;
  KrColor base_ = 0;
  KrColor pane_outline_ = 0;
  KrColor glance_button_ = 0;
  KrColor glance_button_hover_ = 0;
  KrColor glance_button_icon_ = 0;
  KrColor glance_shadow_ = 0;
  KrColor glance_button_shadow_ = 0;

  std::vector<KrGradientLayer> layers_;
  cc::PaintFlags grain_;
  mutable std::vector<cc::PaintFlags> paints_;
  mutable gfx::Size cached_size_;
  mutable bool dirty_ = true;
  bool transparent_ = false;
};
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_BACKDROP_H_
