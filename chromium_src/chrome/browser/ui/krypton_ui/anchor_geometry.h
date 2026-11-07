// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_ANCHOR_GEOMETRY_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_ANCHOR_GEOMETRY_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "base/containers/flat_map.h"
#include "krypton_ui/abi.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"

namespace krypton_ui {

enum class AnchorKind : uint32_t {
  kElement = KR_ANCHOR_ELEMENT,
  kAction = KR_ANCHOR_ACTION,
  kTab = KR_ANCHOR_TAB,
};

struct AnchorKey {
  AnchorKind kind;
  uint64_t id;

  [[nodiscard]] static std::optional<AnchorKey> FromAbi(uint32_t kind,
                                                        uint64_t id);
  auto operator<=>(const AnchorKey&) const = default;
};

// Only the currently visible client-DIP rectangles are retained. In particular,
// removing a tab or hiding compact chrome cannot leave a usable stale anchor.
class AnchorGeometry {
 public:
  [[nodiscard]] bool Update(uint32_t kind,
                            uint64_t id,
                            KrRect bounds,
                            bool visible);
  [[nodiscard]] std::optional<gfx::RectF> Get(AnchorKey key) const;
  [[nodiscard]] size_t size() const { return bounds_.size(); }

 private:
  base::flat_map<AnchorKey, gfx::RectF> bounds_;
};

// Inputs are physical left-to-right DIPs, including under RTL. Clip before
// converting to integer Views bounds, and round outwards so fractional controls
// retain their complete bounds. An empty intersection has no native anchor.
[[nodiscard]] std::optional<gfx::Rect> ResolveAnchorBounds(
    const gfx::RectF& anchor,
    const gfx::RectF& client,
    gfx::PointF client_origin_in_host);

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_ANCHOR_GEOMETRY_H_
