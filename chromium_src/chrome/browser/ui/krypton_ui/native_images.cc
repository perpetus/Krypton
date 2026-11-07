// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/native_images.h"

#include <cmath>
#include <limits>

#include "base/check.h"
#include "base/numerics/safe_conversions.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/image/image_skia_rep.h"

namespace krypton_ui {

NativeImages::NativeImages(WindowSession& session) : session_(session) {}
NativeImages::~NativeImages() {
  Clear();
}

base::expected<KrImageId, std::string> NativeImages::Acquire(
    const ui::ImageModel& image,
    const ui::ColorProvider* colors,
    float scale) {
  if (image.IsEmpty()) {
    return KrImageId{0};
  }
  const auto* api = session_->api();
  if (!api || !std::isfinite(scale) || scale <= 0) {
    return base::unexpected(
        "Cannot publish an image without a native shell and scale.");
  }
  auto raster = image.Rasterize(colors);
  if (raster.isNull()) {
    return base::unexpected("Chromium could not rasterize a native image.");
  }
  const auto& bitmap = raster.GetRepresentation(scale).GetBitmap();
  const Key key{bitmap.getGenerationID(), scale};
  if (const auto found = entries_.find(key); found != entries_.end()) {
    ++found->second.references;
    return found->second.id;
  }
  if (next_id_ == std::numeric_limits<KrImageId>::max()) {
    return base::unexpected("Native image identities are exhausted.");
  }
  SkBitmap pixels;
  if (!pixels.tryAllocPixels(
          SkImageInfo::MakeN32Premul(bitmap.width(), bitmap.height())) ||
      !bitmap.readPixels(pixels.pixmap()) ||
      !base::IsValueInRangeForNumericType<int32_t>(pixels.rowBytes())) {
    return base::unexpected(
        "Cannot convert a native image to premultiplied BGRA.");
  }
  const KrImage value{sizeof(KrImage),
                      ++next_id_,
                      pixels.width(),
                      pixels.height(),
                      base::checked_cast<int32_t>(pixels.rowBytes()),
                      static_cast<const uint8_t*>(pixels.getPixels())};
  api->put_image(api->self, &value);
  entries_.emplace(key, Entry{value.id, 1, std::move(raster)});
  identities_.emplace(value.id, key);
  return value.id;
}

void NativeImages::Release(KrImageId id) {
  if (!id) {
    return;
  }
  const auto identity = identities_.find(id);
  CHECK(identity != identities_.end());
  const auto entry = entries_.find(identity->second);
  CHECK(entry != entries_.end());
  if (--entry->second.references) {
    return;
  }
  if (const auto* api = session_->api()) {
    api->drop_image(api->self, id);
  }
  entries_.erase(entry);
  identities_.erase(identity);
}

void NativeImages::Clear() {
  if (const auto* api = session_->api()) {
    for (const auto& [id, key] : identities_) {
      api->drop_image(api->self, id);
    }
  }
  entries_.clear();
  identities_.clear();
}

}  // namespace krypton_ui
