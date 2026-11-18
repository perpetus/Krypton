// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_PAGE_CAPTURE_ICONS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_PAGE_CAPTURE_ICONS_H_

#include <cstddef>

#include "third_party/skia/include/core/SkPath.h"

namespace krypton_ui::capture_icons {
// Source sizes: the face is 64 units square, the page icons 46 and the action
// glyphs 16. Callers scale and colour the paths.
inline constexpr float kFaceSize = 64;
inline constexpr float kPageIconSize = 46;
inline constexpr float kGlyphSize = 16;

enum class Shape {
  kFaceLines,
  kFullPageViewport,
  kFullPageFrame,
  kFullPageFrameTop,
  kFullPageFrameBottom,
  kFullPageDash,
  kVisibleFrame,
  kVisibleViewport,
  kVisibleDash,
  kClose,
  kCopy,
  kCopyBody,
  kDownload,
};
inline constexpr size_t kShapeCount = static_cast<size_t>(Shape::kDownload) + 1;

// Built once on first use; returned paths are immutable.
const SkPath& Path(Shape shape);
}  // namespace krypton_ui::capture_icons

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_PAGE_CAPTURE_ICONS_H_
