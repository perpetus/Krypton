// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_FRAME_MATERIAL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_FRAME_MATERIAL_H_

#include <cstdint>

#include "ui/views/widget/widget.h"

namespace krypton_ui {

// Widget property read by the upstream Windows frame host (patch 0020). Unset
// keeps upstream behaviour; once set, the host follows the module's request.
inline constexpr char kFrameMaterialProperty[] = "krypton_ui.frame_material";
enum class FrameMaterial : uintptr_t { kUnset, kOpaque, kTransparent };

[[nodiscard]] inline FrameMaterial FrameMaterialOf(
    const views::Widget* widget) {
  return widget ? static_cast<FrameMaterial>(reinterpret_cast<uintptr_t>(
                      widget->GetNativeWindowProperty(kFrameMaterialProperty)))
                : FrameMaterial::kUnset;
}

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_FRAME_MATERIAL_H_
