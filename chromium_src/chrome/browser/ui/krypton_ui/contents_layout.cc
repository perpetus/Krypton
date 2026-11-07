// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/contents_layout.h"

#include <windows.h>

// winnt.h's macro must not rewrite Abseil's StoreFence helper.
#undef StoreFence

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/numerics/safe_conversions.h"
#include "base/trace_event/trace_event.h"
#include "chrome/browser/ui/krypton_ui/content_regions.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_delegate.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "ui/display/win/screen_win.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/view.h"
#include "ui/views/win/hwnd_util.h"

namespace krypton_ui {
namespace {

// Native module coordinates are physical left-to-right client DIPs. Views
// mirrors child positions for RTL, so mirror the stored child rectangle once
// to keep its physical position equal to the rectangle reported by the module.
void SetPhysicalBounds(views::View* view, gfx::Rect bounds) {
  if (view && view->parent()) {
    bounds.set_x(view->parent()->GetMirroredXForRect(bounds));
    view->SetBoundsRect(bounds);
  }
}

}  // namespace

ContentsLayout::ContentsLayout(
    std::unique_ptr<BrowserViewLayoutDelegate> delegate,
    BrowserViewLayoutViews views,
    base::WeakPtr<WindowSession> session,
    base::RepeatingCallback<uint32_t()> window_flags,
    base::RepeatingCallback<void(std::string)> failure)
    : BrowserViewLayout(std::move(delegate), std::move(views)),
      session_(std::move(session)),
      window_flags_(std::move(window_flags)),
      failure_(std::move(failure)) {
  CHECK(window_flags_);
  CHECK(failure_);
  CHECK(this->views().multi_contents_view);
}

ContentsLayout::~ContentsLayout() = default;

void ContentsLayout::Layout(views::View* host) {
  TRACE_EVENT0("browser", "KryptonUI.LayoutContents");
  if (!session_ || failed_) {
    return;
  }
  const auto weak = weak_factory_.GetWeakPtr();
  const auto flags = window_flags_.Run();
  if (!weak || !session_) {
    return;
  }
  auto result = session_->UpdateWindowState(flags);
  if (!weak || !session_) {
    return;
  }
  if (!result.has_value()) {
    failed_ = true;
    // The callback can close and destroy the browser. Do not touch members
    // after reporting a terminal native-window error.
    failure_.Run(std::move(result.error()));
    return;
  }
  const auto window = views::HWNDForView(host);
  POINT origin{};
  if (!window || !::ClientToScreen(window, &origin)) {
    failed_ = true;
    failure_.Run("Cannot convert Krypton's native client origin.");
    return;
  }
  const auto screen_origin =
      display::win::GetScreenWin()
          ->ScreenToDIPRect(window, gfx::Rect(origin.x, origin.y, 0, 0))
          .origin();
  const auto host_origin = host->GetBoundsInScreen().origin();
  const auto module_content = session_->geometry().content();
  gfx::RectF native_content(module_content.x, module_content.y,
                            module_content.width, module_content.height);
  native_content.Offset(screen_origin.x() - host_origin.x(),
                        screen_origin.y() - host_origin.y());
  auto available = gfx::ToEnclosedRect(native_content);
  available.Intersect(host->GetLocalBounds());

  SetPhysicalBounds(views().window_scrim, host->GetLocalBounds());
  SetPhysicalBounds(views().main_background_region, host->GetLocalBounds());
  SetPhysicalBounds(views().main_shadow_overlay, {});
  SetPhysicalBounds(views().top_container_separator, {});
  SetPhysicalBounds(views().side_panel_content_transition_scrim, available);

  auto* panel = views().side_panel.get();
  int panel_width = 0;
  if (panel && panel->GetVisible()) {
    const auto desired = std::max(panel->GetMinimumSize().width(),
                                  panel->GetPreferredSize().width());
    panel_width =
        std::clamp(base::ClampRound<int>(desired * panel->GetAnimationValue()),
                   0, available.width());
  }
  const int service_width = available.width() - panel_width;
  const int bookmark_height =
      views().bookmark_bar && delegate().IsBookmarkBarVisible()
          ? views().bookmark_bar->GetHeightForWidth(service_width)
          : 0;
  const int infobar_height =
      views().infobar_container && delegate().IsInfobarVisible()
          ? views().infobar_container->GetHeightForWidth(service_width)
          : 0;
  const auto regions =
      PartitionContent(available, panel_width, panel && panel->IsRightAligned(),
                       bookmark_height, infobar_height);
  if (panel && panel->GetVisible()) {
    SetPhysicalBounds(panel, regions.side_panel);
  }
  if (views().top_container) {
    SetPhysicalBounds(views().top_container, regions.bookmarks);
    if (views().bookmark_bar &&
        views().bookmark_bar->parent() == views().top_container) {
      SetViewVisibility(views().bookmark_bar, !regions.bookmarks.IsEmpty());
      SetPhysicalBounds(views().bookmark_bar,
                        gfx::Rect(regions.bookmarks.size()));
    }
  }
  if (views().infobar_container) {
    SetViewVisibility(views().infobar_container, !regions.infobars.IsEmpty());
    SetPhysicalBounds(views().infobar_container, regions.infobars);
  }

  contents_bounds_ = regions.page;
  auto* contents = views().multi_contents_view.get();
  contents->SetShouldShowTopSeparator(false);
  contents->SetBackgroundRadii(
      gfx::RoundedCornersF(session_->geometry().corner_radius()));
  SetPhysicalBounds(contents, contents_bounds_);
  // MultiContentsView retains WebContents, docked DevTools and their clipping.
  // Existing bubble/dialog hosts continue reading the final content bounds.
  UpdateBubbles();
}

gfx::Size ContentsLayout::GetMinimumSize(const views::View* host) const {
  if (!session_) {
    return {};
  }
  const auto& metrics = session_->geometry();
  gfx::Size minimum(base::ClampCeil<int>(metrics.minimum_width()),
                    base::ClampCeil<int>(metrics.minimum_height()));
  if (views().side_panel && views().side_panel->GetVisible()) {
    minimum.Enlarge(views().side_panel->GetMinimumSize().width(), 0);
  }
  if (views().bookmark_bar && delegate().IsBookmarkBarVisible()) {
    minimum.Enlarge(0, views().bookmark_bar->GetPreferredSize().height());
  }
  if (views().infobar_container && delegate().IsInfobarVisible()) {
    minimum.Enlarge(0, views().infobar_container->GetPreferredSize().height());
  }
  return minimum;
}

gfx::Point ContentsLayout::GetDialogPosition(
    const gfx::Size& dialog_size) const {
  return gfx::Point(
      contents_bounds_.x() +
          std::max(0, (contents_bounds_.width() - dialog_size.width()) / 2),
      contents_bounds_.y());
}

gfx::Size ContentsLayout::GetMaximumDialogSize() const {
  return contents_bounds_.size();
}

}  // namespace krypton_ui
