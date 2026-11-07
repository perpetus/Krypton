// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/frame_view.h"

#include <windows.h>

#include <algorithm>
#include <utility>

#include "base/numerics/safe_conversions.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "ui/base/hit_test.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/win/hwnd_util.h"
#include "ui/views/window/client_view.h"

namespace krypton_ui {

FrameView::FrameView(BrowserWidget* widget,
                     BrowserView* view,
                     base::WeakPtr<WindowSession> session)
    : BrowserFrameView(widget, view), session_(std::move(session)) {}

FrameView::~FrameView() = default;

int FrameView::GetTopInset(bool restored) const {
  return 0;
}

void FrameView::UpdateThrobber(bool running) {
  // Tab loading state is published through the module's tab/navigation ports.
}

void FrameView::UpdateMinimumSize() {
  if (!session_) {
    return;
  }
  const auto& geometry = session_->geometry();
  const gfx::Size minimum(base::ClampCeil<int>(geometry.minimum_width()),
                          base::ClampCeil<int>(geometry.minimum_height()));
  if (minimum != minimum_) {
    minimum_ = minimum;
    browser_widget()->OnSizeConstraintsChanged();
  }
}

gfx::Size FrameView::GetMinimumSize() const {
  auto minimum = minimum_;
  if (auto* client = browser_widget()->client_view()) {
    minimum.SetToMax(client->GetMinimumSize());
  }
  return GetWindowBoundsForClientBounds(gfx::Rect(minimum)).size();
}

gfx::Rect FrameView::GetBoundsForClientView() const {
  return GetLocalBounds();
}

gfx::Rect FrameView::GetWindowBoundsForClientBounds(
    const gfx::Rect& client_bounds) const {
  const HWND window = views::HWNDForWidget(browser_widget());
  RECT native_client{}, native_window{};
  POINT client_origin{};
  if (!window || !::GetClientRect(window, &native_client) ||
      !::GetWindowRect(window, &native_window) ||
      !::ClientToScreen(window, &client_origin)) {
    return client_bounds;
  }
  const auto dpi = ::GetDpiForWindow(window);
  if (!dpi) {
    return client_bounds;
  }
  const float scale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
  // Read the actual native frame after titlebar extension, rather than adding
  // an assumed caption or resize-border size from a different frame mode.
  const int horizontal = std::max(
      0L, native_window.right - native_window.left - native_client.right);
  const int vertical = std::max(
      0L, native_window.bottom - native_window.top - native_client.bottom);
  auto result = client_bounds;
  result.Offset(
      -base::ClampCeil<int>((client_origin.x - native_window.left) / scale),
      -base::ClampCeil<int>((client_origin.y - native_window.top) / scale));
  result.set_size(gfx::Size(
      base::ClampCeil<int>(client_bounds.width() + horizontal / scale),
      base::ClampCeil<int>(client_bounds.height() + vertical / scale)));
  return result;
}

int FrameView::NonClientHitTest(const gfx::Point& point) {
  // The module's earlier HWND subclass already handled captions and resizing.
  return GetLocalBounds().Contains(point) ? HTCLIENT : HTNOWHERE;
}

BEGIN_METADATA(FrameView)
END_METADATA

}  // namespace krypton_ui
