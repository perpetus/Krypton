// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include <optional>
#include <utility>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "content/public/common/drop_data.h"
#include "ui/base/clipboard/clipboard_url_info.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/point_f.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {
std::optional<GURL> ReadUrl(const ui::OSExchangeData& data) {
  // Never turn a local file into a file: URL or interpret arbitrary drop text
  // as omnibox input. Navigation retains the browser's policy checks.
  const auto urls =
      data.GetURLs(ui::FilenameToURLPolicy::DO_NOT_CONVERT_FILENAMES);
  if (!urls.empty() && urls.front().url.is_valid() &&
      urls.front().url.SchemeIsHTTPOrHTTPS())
    return urls.front().url;
  if (auto text = data.GetString()) {
    GURL url(*text);
    if (url.is_valid() && url.SchemeIsHTTPOrHTTPS() &&
        text->find_first_of(u"\r\n\t") == std::u16string::npos)
      return url;
  }
  return std::nullopt;
}
}  // namespace
bool BrowserAdapter::CanDropUrl(const ui::OSExchangeData& data) {
  return ReadUrl(data).has_value();
}
bool BrowserAdapter::UrlDragChanged(const gfx::PointF& point, bool active) {
  url_drag_preview_active_ = active;
  const auto* api = session_.api();
  return api && KR_HAS_SLOT(api, KrShellUI, url_drag_changed) &&
         api->url_drag_changed &&
         api->url_drag_changed(api->self, point.x(), point.y(), active);
}
void BrowserAdapter::UrlDragExited() {
  // Leaving the page/root target can mean entering a revealed XAML island.
  // Keep the reveal during that handoff; the terminal drag-ended callback
  // still clears it unconditionally.
  if (!url_drag_preview_active_)
    return;
  auto* screen = display::Screen::Get();
  if (!screen) {
    UrlDragChanged({}, false);
    return;
  }
  auto point = screen->GetCursorScreenPoint();
  views::View::ConvertPointFromScreen(&*view_, &point);
  if (!UrlDragChanged(gfx::PointF(point), true))
    UrlDragChanged({}, false);
}
bool BrowserAdapter::PreviewUrlDrag(const content::DropData& data,
                                    const gfx::PointF& point) {
  const bool valid =
      (!data.url_infos.empty() && data.url_infos.front().url.is_valid() &&
       data.url_infos.front().url.SchemeIsHTTPOrHTTPS()) ||
      (data.text && GURL(*data.text).is_valid() &&
       GURL(*data.text).SchemeIsHTTPOrHTTPS());
  return UrlDragChanged(point, valid);
}
base::OnceClosure BrowserAdapter::CaptureUrlDrop(const ui::OSExchangeData& data,
                                                 const gfx::PointF& point) {
  const auto url = ReadUrl(data);
  const bool accept = url && UrlDragChanged(point, true);
  UrlDragChanged(point, false);
  if (!accept)
    return {};
  return base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> owner, GURL url, KrSpaceId space) {
        if (!owner || !owner->workspaces_.Contains(space))
          return;
        std::ignore = owner->session_.Defer(base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner, GURL url, KrSpaceId space) {
              if (!owner || !owner->workspaces_.Contains(space))
                return;
              const base::AutoReset<KrSpaceId> workspace(&owner->new_tab_space_,
                                                         space);
              NavigateParams params(&*owner->browser_, url,
                                    ui::PAGE_TRANSITION_LINK);
              params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
              params.window_action = NavigateParams::WindowAction::kShowWindow;
              Navigate(&params);
            },
            owner, std::move(url), space));
      },
      weak_factory_.GetWeakPtr(), *url, active_space_);
}
}  // namespace krypton_ui
