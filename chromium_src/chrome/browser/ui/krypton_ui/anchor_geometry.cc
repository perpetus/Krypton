// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/anchor_geometry.h"

#include <cmath>

#include "ui/gfx/geometry/rect_conversions.h"

namespace krypton_ui {
namespace {

bool ValidRect(const gfx::RectF& rect) {
  return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
         std::isfinite(rect.width()) && std::isfinite(rect.height()) &&
         std::isfinite(rect.right()) && std::isfinite(rect.bottom()) &&
         rect.width() >= 0 && rect.height() >= 0;
}

}  // namespace

std::optional<AnchorKey> AnchorKey::FromAbi(uint32_t kind, uint64_t id) {
  switch (kind) {
    case KR_ANCHOR_ELEMENT:
    case KR_ANCHOR_ACTION:
    case KR_ANCHOR_TAB:
      return AnchorKey{static_cast<AnchorKind>(kind), id};
    default:
      return std::nullopt;
  }
}

bool AnchorGeometry::Update(uint32_t kind,
                            uint64_t id,
                            KrRect bounds,
                            bool visible) {
  const auto key = AnchorKey::FromAbi(kind, id);
  if (!key) {
    return false;
  }
  if (!visible) {
    return bounds_.erase(*key) != 0;
  }
  // RectF normalizes negative sizes, so validate the ABI values first.
  if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) ||
      !std::isfinite(bounds.width) || !std::isfinite(bounds.height) ||
      bounds.width < 0 || bounds.height < 0) {
    return false;
  }
  const gfx::RectF rect(bounds.x, bounds.y, bounds.width, bounds.height);
  if (!ValidRect(rect)) {
    return false;
  }
  if (rect.IsEmpty()) {
    return bounds_.erase(*key) != 0;
  }
  const auto found = bounds_.find(*key);
  if (found != bounds_.end() && found->second == rect) {
    return false;
  }
  bounds_.insert_or_assign(*key, rect);
  return true;
}

std::optional<gfx::RectF> AnchorGeometry::Get(AnchorKey key) const {
  const auto found = bounds_.find(key);
  return found == bounds_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<gfx::Rect> ResolveAnchorBounds(
    const gfx::RectF& anchor,
    const gfx::RectF& client,
    gfx::PointF client_origin_in_host) {
  if (!ValidRect(anchor) || !ValidRect(client) ||
      !std::isfinite(client_origin_in_host.x()) ||
      !std::isfinite(client_origin_in_host.y())) {
    return std::nullopt;
  }
  auto clipped = anchor;
  clipped.Intersect(client);
  if (clipped.IsEmpty()) {
    return std::nullopt;
  }
  clipped.Offset(client_origin_in_host.OffsetFromOrigin());
  if (!ValidRect(clipped)) {
    return std::nullopt;
  }
  return gfx::ToEnclosingRect(clipped);
}

}  // namespace krypton_ui
