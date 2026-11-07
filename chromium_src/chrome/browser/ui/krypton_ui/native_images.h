// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_IMAGES_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_IMAGES_H_

#include <map>
#include <string>
#include <utility>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "krypton_ui/abi.h"
#include "ui/gfx/image/image_skia.h"

namespace ui {
class ColorProvider;
class ImageModel;
}  // namespace ui

namespace krypton_ui {
class WindowSession;

// Window-scoped leases on Chromium's resource identities at a device scale.
// The module receives each live bitmap once and drops it after its last user.
class NativeImages {
 public:
  explicit NativeImages(WindowSession& session);
  ~NativeImages();
  NativeImages(const NativeImages&) = delete;
  NativeImages& operator=(const NativeImages&) = delete;

  [[nodiscard]] base::expected<KrImageId, std::string> Acquire(
      const ui::ImageModel& image,
      const ui::ColorProvider* colors,
      float scale);
  void Release(KrImageId id);
  void Clear();
  [[nodiscard]] base::WeakPtr<NativeImages> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  using Key = std::pair<uint32_t, float>;
  struct Entry {
    KrImageId id;
    size_t references;
    gfx::ImageSkia source;
  };
  const raw_ref<WindowSession> session_;
  std::map<Key, Entry> entries_;
  base::flat_map<KrImageId, Key> identities_;
  KrImageId next_id_ = 0;
  base::WeakPtrFactory<NativeImages> weak_factory_{this};
};
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_IMAGES_H_
