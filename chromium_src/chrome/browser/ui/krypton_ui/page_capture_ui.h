// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_PAGE_CAPTURE_UI_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_PAGE_CAPTURE_UI_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/krypton_ui/page_capture.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"

namespace content {
class WebContents;
}
namespace views {
class Widget;
}
namespace krypton_ui {
class CaptureKeyHandler;
class CaptureOverlayView;

// Browser-owned, transient overlay above the page, styled after Zen's capture
// overlay: a dimmed page with a face whose eyes follow the pointer, element
// hover, drag selection with handles, and visible/full-page shortcuts. Neither
// the page nor an injected script can impersonate these controls or read the
// captured image. It paints only on input and runs no timers while idle.
class PageCaptureUI {
 public:
  PageCaptureUI(
      content::WebContents& contents,
      PageCaptureOptions options,
      base::RepeatingCallback<void(gfx::Point)> hover,
      base::RepeatingCallback<void(gfx::Rect, PageCaptureAction)> region,
      base::RepeatingClosure full_page,
      base::RepeatingClosure cancel);
  ~PageCaptureUI();

  // Highlights the element under the pointer; null clears the highlight.
  void SetHover(std::optional<gfx::Rect> bounds);
  void SetFullPageUnavailable(const std::u16string& reason);
  void Hide();

 private:
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<CaptureOverlayView> overlay_ = nullptr;
  std::unique_ptr<CaptureKeyHandler> keys_;
};
}  // namespace krypton_ui
#endif
