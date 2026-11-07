// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_GLASS_BACKDROP_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_GLASS_BACKDROP_H_

#include <memory>

#include "krypton_ui/abi.h"

class BrowserView;

namespace krypton_ui {
class WindowSession;

// Copies the browser compositor's background into shared BGRA GPU textures.
// WinUI islands are separate native surfaces and cannot enter this source.
class NativeGlassBackdrop {
 public:
  NativeGlassBackdrop(BrowserView& view, WindowSession& session);
  ~NativeGlassBackdrop();
  NativeGlassBackdrop(const NativeGlassBackdrop&) = delete;
  NativeGlassBackdrop& operator=(const NativeGlassBackdrop&) = delete;
  void Request(const KrGlassBackdropRequest& request);
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_GLASS_BACKDROP_H_
