// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_FRAME_VIEW_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_FRAME_VIEW_H_

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/views/frame/browser_frame_view.h"
#include "ui/base/metadata/metadata_header_macros.h"

namespace krypton_ui {

class WindowSession;

// A transparent Views frame. KryptonUI owns the native caption configuration
// and HWND hit-test subclass; this frame adds no duplicate controls or drag
// hit testing. Web apps and auxiliary windows continue using their own frame.
class FrameView final : public BrowserFrameView {
  METADATA_HEADER(FrameView, BrowserFrameView)

 public:
  FrameView(BrowserWidget* widget,
            BrowserView* view,
            base::WeakPtr<WindowSession> session);
  ~FrameView() override;

  int GetTopInset(bool restored) const override;
  void UpdateThrobber(bool running) override;
  void UpdateMinimumSize() override;
  gfx::Size GetMinimumSize() const override;
  gfx::Rect GetBoundsForClientView() const override;
  gfx::Rect GetWindowBoundsForClientBounds(
      const gfx::Rect& client_bounds) const override;
  int NonClientHitTest(const gfx::Point& point) override;

 private:
  const base::WeakPtr<WindowSession> session_;
  gfx::Size minimum_;
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_FRAME_VIEW_H_
