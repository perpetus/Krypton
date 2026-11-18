// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/page_capture.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/devtools/devtools_availability_checker.h"
#include "chrome/browser/media/webrtc/capture_policy_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/krypton_ui/page_capture_ui.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/snapshot/snapshot.h"

namespace krypton_ui {
namespace {
constexpr size_t kMaxMessageBytes = 64 * 1024 * 1024;
constexpr size_t kMaxSmallMessageBytes = 512 * 1024;
constexpr double kMaxPixels = 32 * 1024 * 1024;
constexpr double kMaxDimension = 32768;
// The reference overlay ignores elements smaller than this, in CSS pixels.
constexpr double kMinHoverWidth = 30;
constexpr double kMinHoverHeight = 10;
constexpr char kCaptureError[] =
    "Could not capture this page. Try selecting a smaller area.";
constexpr char kTooLarge[] =
    "This capture is too large. Select a smaller area instead.";

bool Fits(double width, double height, double scale) {
  width = std::ceil(width * scale);
  height = std::ceil(height * scale);
  return std::isfinite(width) && std::isfinite(height) && scale > 0 &&
         width > 0 && height > 0 && width <= kMaxDimension &&
         height <= kMaxDimension && width * height <= kMaxPixels;
}

// Parse and decode potentially large responses away from the browser UI thread.
// Check IHDR before allocating a bitmap, even though Chromium produced the PNG.
SkBitmap DecodeCapture(std::string message, int id) {
  auto json = base::JSONReader::ReadDict(message, base::JSON_PARSE_RFC);
  if (!json || json->FindInt("id") != id || json->contains("error"))
    return {};
  const auto* data = json->FindStringByDottedPath("result.data");
  if (!data || data->size() > kMaxMessageBytes)
    return {};
  auto png = base::Base64Decode(*data);
  constexpr uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (!png || png->size() < 24 ||
      !std::equal(std::begin(signature), std::end(signature), png->begin()) ||
      std::string_view(reinterpret_cast<const char*>(png->data() + 12), 4) !=
          "IHDR")
    return {};
  auto dimension = [&](size_t offset) {
    uint32_t value = 0;
    for (size_t i = offset; i < offset + 4; ++i)
      value = (value << 8) | (*png)[i];
    return value;
  };
  if (!Fits(dimension(16), dimension(20), 1))
    return {};
  return gfx::PNGCodec::Decode(*png);
}
}  // namespace

PageCapture::PageCapture(tabs::TabInterface& tab)
    : content::WebContentsObserver(tab.GetContents()),
      url_(tab.GetContents()->GetLastCommittedURL()) {
  deactivated_ = tab.RegisterWillDeactivate(base::BindRepeating(
      [](base::WeakPtr<PageCapture> self, tabs::TabInterface*) {
        if (self)
          self->Finish();
      },
      weak_factory_.GetWeakPtr()));
  detached_ = tab.RegisterWillDetach(base::BindRepeating(
      [](base::WeakPtr<PageCapture> self, tabs::TabInterface*,
         tabs::TabInterface::DetachReason) {
        if (self)
          self->Finish();
      },
      weak_factory_.GetWeakPtr()));
}

PageCapture::~PageCapture() {
  weak_factory_.InvalidateWeakPtrs();
  ui_.reset();
  Detach();
}

bool PageCapture::Allowed() const {
  auto* contents = web_contents();
  return contents && !contents->IsCrashed() &&
         contents->GetLastCommittedURL() == url_ &&
         contents->GetVisibility() == content::Visibility::VISIBLE &&
         url_.SchemeIsHTTPOrHTTPS() &&
         capture_policy::GetAllowedCaptureLevel(url_, contents) !=
             AllowedScreenCaptureLevel::kDisallowed;
}

void PageCapture::Start(PageCaptureMode mode,
                        PageCaptureOptions options,
                        Completion completion) {
  completion_ = base::BindPostTaskToCurrentDefault(std::move(completion));
  options_ = options;
  action_ = options.add_to_easel ? PageCaptureAction::kEasel
                                 : PageCaptureAction::kPreview;
  if (!Allowed()) {
    Finish({}, "Screenshot capture is unavailable on this page.");
    return;
  }
  if (mode == PageCaptureMode::kFullPage) {
    CaptureFullPage();
    return;
  }
  // Post actions past the native button's dispatch before replacing its view.
  const auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  const auto weak = weak_factory_.GetWeakPtr();
  ui_ = std::make_unique<PageCaptureUI>(
      *web_contents(), options,
      base::BindPostTask(runner,
                         base::BindRepeating(&PageCapture::HoverAt, weak)),
      base::BindPostTask(
          runner, base::BindRepeating(&PageCapture::CaptureRegion, weak)),
      base::BindPostTask(
          runner, base::BindRepeating(&PageCapture::CaptureFullPage, weak)),
      base::BindPostTask(runner, base::BindRepeating(
                                     [](base::WeakPtr<PageCapture> self) {
                                       if (self)
                                         self->Finish();
                                     },
                                     weak)));
  // Element hover and full-page capture need a transient inspector session.
  // Without one, the overlay still selects regions and the visible page.
  std::string error;
  if (!Attach(&error)) {
    ui_->SetFullPageUnavailable(base::UTF8ToUTF16(error));
    return;
  }
  Send("Page.getLayoutMetrics", {},
       base::BindOnce(&PageCapture::ReadViewport, weak));
}

bool PageCapture::Attach(std::string* error) {
  if (attached_)
    return true;
  if (!IsInspectionAllowed(
          Profile::FromBrowserContext(web_contents()->GetBrowserContext()),
          web_contents())) {
    *error = "Full-page capture is disabled by your administrator.";
    return false;
  }
  agent_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents());
  // Never interfere with a developer's existing inspector or its emulation.
  if (agent_->IsAttached() || !(attached_ = agent_->AttachClient(this))) {
    agent_.reset();
    *error = "Close DevTools or the debugger to capture the full page.";
    return false;
  }
  return true;
}

void PageCapture::ReadViewport(const base::DictValue& result) {
  const auto* viewport = result.FindDict("cssVisualViewport");
  if (!viewport)
    return;
  const auto x = viewport->FindDouble("pageX");
  const auto y = viewport->FindDouble("pageY");
  // Browser zoom and pinch zoom both scale CSS pixels into the overlay's DIPs.
  const double scale = blink::ZoomLevelToZoomFactor(
                           content::HostZoomMap::GetZoomLevel(web_contents())) *
                       viewport->FindDouble("zoom").value_or(1);
  if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y) ||
      !std::isfinite(scale) || scale <= 0)
    return;
  page_x_ = *x;
  page_y_ = *y;
  css_to_dip_ = scale;
  viewport_ready_ = true;
  if (hover_point_ && !hover_id_)
    QueryHover();
}

void PageCapture::HoverAt(gfx::Point point) {
  if (finished_ || capturing_)
    return;
  hover_point_ = point;
  if (viewport_ready_ && !hover_id_)
    QueryHover();
}

void PageCapture::QueryHover() {
  hover_sent_ = *hover_point_;
  // DOM.getNodeForLocation takes document coordinates in CSS pixels.
  const double x = page_x_ + hover_sent_.x() / css_to_dip_;
  const double y = page_y_ + hover_sent_.y() / css_to_dip_;
  SendHover(
      "DOM.getNodeForLocation",
      base::DictValue()
          .Set("x", base::ClampRound(x))
          .Set("y", base::ClampRound(y)),
      base::BindOnce(&PageCapture::HoverNode, weak_factory_.GetWeakPtr()));
}

void PageCapture::HoverNode(const base::DictValue* result) {
  const std::optional<int> node =
      result ? result->FindInt("backendNodeId") : std::nullopt;
  if (!node) {
    HoverDone(std::nullopt);
    return;
  }
  SendHover("DOM.getBoxModel", base::DictValue().Set("backendNodeId", *node),
            base::BindOnce(&PageCapture::HoverBox, weak_factory_.GetWeakPtr()));
}

void PageCapture::HoverBox(const base::DictValue* result) {
  const auto* quad =
      result ? result->FindListByDottedPath("model.border") : nullptr;
  if (!quad || quad->size() != 8) {
    HoverDone(std::nullopt);
    return;
  }
  double left = std::numeric_limits<double>::infinity(), top = left;
  double right = -left, bottom = right;
  for (size_t i = 0; i < 8; i += 2) {
    auto x = (*quad)[i].GetIfDouble(), y = (*quad)[i + 1].GetIfDouble();
    if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
      HoverDone(std::nullopt);
      return;
    }
    left = std::min(left, *x);
    right = std::max(right, *x);
    top = std::min(top, *y);
    bottom = std::max(bottom, *y);
  }
  if (right - left < kMinHoverWidth || bottom - top < kMinHoverHeight) {
    HoverDone(std::nullopt);
    return;
  }
  // Box-model quads are CSS pixels relative to the visual viewport.
  gfx::RectF css(left, top, right - left, bottom - top);
  css.Scale(css_to_dip_);
  gfx::Rect bounds = gfx::ToEnclosingRect(css);
  const gfx::Rect view(web_contents()->GetViewBounds().size());
  bounds.Intersect(view);
  // Skip the page root and other near-viewport wrappers, like the reference.
  if (bounds.IsEmpty() ||
      bounds.size().Area64() * 10 >= view.size().Area64() * 9) {
    HoverDone(std::nullopt);
    return;
  }
  HoverDone(bounds);
}

void PageCapture::HoverDone(std::optional<gfx::Rect> bounds) {
  if (finished_ || capturing_ || !ui_)
    return;
  ui_->SetHover(bounds);
  if (hover_point_ && *hover_point_ != hover_sent_)
    QueryHover();
}

void PageCapture::CaptureRegion(gfx::Rect bounds, PageCaptureAction action) {
  if (finished_ || capturing_ || !Allowed())
    return;
  auto* view = web_contents()->GetRenderWidgetHostView();
  bounds.Intersect(gfx::Rect(web_contents()->GetViewBounds().size()));
  if (!view || bounds.width() < 2 || bounds.height() < 2)
    return;
  if (!Fits(bounds.width(), bounds.height(), view->GetDeviceScaleFactor())) {
    Finish({}, kTooLarge);
    return;
  }
  // Easel captures always go to Easel; ordinary captures never do.
  if (options_.add_to_easel)
    action = PageCaptureAction::kEasel;
  else if (action == PageCaptureAction::kEasel)
    action = PageCaptureAction::kPreview;
  action_ = action;
  capturing_ = true;
  hover_point_.reset();
  if (ui_)
    ui_->Hide();
  // The region snapshot needs no inspector session.
  Detach();
  // Snapshot the page's own surface so the overlay can never appear in it.
  ui::GrabViewSnapshot(
      view->GetNativeView(), bounds,
      base::BindOnce(
          [](base::WeakPtr<PageCapture> self, gfx::Image image) {
            if (self && self->Allowed())
              self->Finish(image, image.IsEmpty() ? kCaptureError : "");
            else if (self)
              self->Finish();
          },
          weak_factory_.GetWeakPtr()));
}

void PageCapture::CaptureFullPage() {
  if (finished_ || capturing_ || !Allowed())
    return;
  std::string error;
  if (!Attach(&error)) {
    Finish({}, error);
    return;
  }
  action_ = options_.add_to_easel ? PageCaptureAction::kEasel
                                  : PageCaptureAction::kPreview;
  hover_point_.reset();
  if (ui_)
    ui_->Hide();
  Send("Page.getLayoutMetrics", {},
       base::BindOnce(&PageCapture::ReadMetrics, weak_factory_.GetWeakPtr()));
}

void PageCapture::Send(std::string_view method,
                       base::DictValue params,
                       Reply reply) {
  if (finished_ || !Allowed() || !attached_) {
    Finish();
    return;
  }
  pending_id_ = ++next_id_;
  reply_ = std::move(reply);
  auto message = base::WriteJson(base::DictValue()
                                     .Set("id", pending_id_)
                                     .Set("method", method)
                                     .Set("params", std::move(params)));
  if (!message) {
    Finish({}, kCaptureError);
    return;
  }
  deadline_.Start(
      FROM_HERE, base::Seconds(30),
      base::BindOnce(
          [](base::WeakPtr<PageCapture> self) {
            if (self)
              self->Finish({},
                           "Screenshot capture timed out. Please try again.");
          },
          weak_factory_.GetWeakPtr()));
  agent_->DispatchProtocolMessage(this, base::as_byte_span(*message));
}

// Hover queries are pointer-driven and optional: they never time out or end the
// capture, and a failed query only clears the highlight.
void PageCapture::SendHover(std::string_view method,
                            base::DictValue params,
                            HoverReply reply) {
  if (finished_ || capturing_ || !attached_)
    return;
  auto message = base::WriteJson(base::DictValue()
                                     .Set("id", next_id_ + 1)
                                     .Set("method", method)
                                     .Set("params", std::move(params)));
  if (!message)
    return;
  hover_id_ = ++next_id_;
  hover_reply_ = std::move(reply);
  agent_->DispatchProtocolMessage(this, base::as_byte_span(*message));
}

void PageCapture::ReadMetrics(const base::DictValue& result) {
  const auto* size = result.FindDict("cssContentSize");
  const auto* viewport = result.FindDict("cssVisualViewport");
  if (!size || !viewport) {
    Finish({}, kCaptureError);
    return;
  }
  auto x = size->FindDouble("x"), y = size->FindDouble("y");
  auto width = size->FindDouble("width"), height = size->FindDouble("height");
  const double zoom = viewport->FindDouble("zoom").value_or(1);
  if (!x || !y || !width || !height || !std::isfinite(*x) ||
      !std::isfinite(*y)) {
    Finish({}, kCaptureError);
    return;
  }
  if (!Fits(*width, *height, zoom)) {
    Finish({}, kTooLarge);
    return;
  }
  Capture(gfx::RectF(*x, *y, *width, *height), zoom);
}

void PageCapture::Capture(gfx::RectF bounds, double zoom) {
  auto* view =
      web_contents() ? web_contents()->GetRenderWidgetHostView() : nullptr;
  if (!view || !std::isfinite(zoom) ||
      !Fits(bounds.width(), bounds.height(),
            zoom * view->GetDeviceScaleFactor())) {
    Finish({}, kTooLarge);
    return;
  }
  bounds.Scale(zoom);
  // Page.captureScreenshot temporarily changes these two preferences. Its
  // normal reply restores them, but a canceled/detached request may not reply.
  // Preserve only those fields, never overwrite unrelated preference updates.
  const auto& preferences = web_contents()->GetOrCreateWebPreferences();
  original_hide_scrollbars_ = preferences.hide_scrollbars;
  original_record_whole_document_ = preferences.record_whole_document;
  preferences_saved_ = true;
  auto clip = base::DictValue()
                  .Set("x", double(bounds.x()))
                  .Set("y", double(bounds.y()))
                  .Set("width", double(bounds.width()))
                  .Set("height", double(bounds.height()))
                  .Set("scale", 1);
  capturing_ = true;
  Send("Page.captureScreenshot",
       base::DictValue()
           .Set("format", "png")
           .Set("fromSurface", true)
           .Set("captureBeyondViewport", true)
           .Set("optimizeForSpeed", true)
           .Set("clip", std::move(clip)),
       {});
}

void PageCapture::DispatchProtocolMessage(content::DevToolsAgentHost*,
                                          base::span<const uint8_t> message) {
  if (finished_)
    return;
  if (message.size() >
      (capturing_ ? kMaxMessageBytes : kMaxSmallMessageBytes)) {
    Finish({}, kCaptureError);
    return;
  }
  const std::string_view text(reinterpret_cast<const char*>(message.data()),
                              message.size());
  // Replies are small except the image. The sole large response is parsed on
  // a worker.
  if (capturing_ && message.size() > kMaxSmallMessageBytes) {
    capturing_ = false;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&DecodeCapture, std::string(text), pending_id_),
        base::BindOnce(&PageCapture::Decoded, weak_factory_.GetWeakPtr()));
    return;
  }
  auto json = base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
  if (!json) {
    Finish({}, kCaptureError);
    return;
  }
  // No protocol domains are enabled, so events are ignored.
  const auto id = json->FindInt("id");
  if (!id)
    return;
  if (*id == hover_id_) {
    hover_id_ = 0;
    if (auto reply = std::move(hover_reply_)) {
      std::move(reply).Run(json->contains("error") ? nullptr
                                                   : json->FindDict("result"));
    }
    return;
  }
  if (*id != pending_id_)
    return;
  deadline_.Stop();
  if (json->contains("error")) {
    Finish({}, kCaptureError);
    return;
  }
  if (capturing_) {
    capturing_ = false;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&DecodeCapture, std::string(text), *id),
        base::BindOnce(&PageCapture::Decoded, weak_factory_.GetWeakPtr()));
  } else if (reply_) {
    const auto* result = json->FindDict("result");
    if (!result) {
      Finish({}, kCaptureError);
      return;
    }
    std::move(reply_).Run(*result);
  }
}

void PageCapture::Decoded(SkBitmap bitmap) {
  if (finished_)
    return;
  if (!Allowed()) {
    Finish();
    return;
  }
  if (bitmap.drawsNothing()) {
    Finish({}, kCaptureError);
    return;
  }
  Finish(gfx::Image::CreateFrom1xBitmap(bitmap));
}

void PageCapture::Detach() {
  hover_id_ = 0;
  hover_reply_.Reset();
  if (attached_) {
    attached_ = false;
    agent_->DetachClient(this);
  }
  agent_.reset();
  if (std::exchange(preferences_saved_, false) && web_contents() &&
      !web_contents()->IsBeingDestroyed()) {
    auto preferences = web_contents()->GetOrCreateWebPreferences();
    if (preferences.hide_scrollbars != original_hide_scrollbars_ ||
        preferences.record_whole_document != original_record_whole_document_) {
      preferences.hide_scrollbars = original_hide_scrollbars_;
      preferences.record_whole_document = original_record_whole_document_;
      web_contents()->SetWebPreferences(preferences);
    }
  }
}

void PageCapture::Finish(gfx::Image image, std::string error) {
  if (finished_)
    return;
  finished_ = true;
  deadline_.Stop();
  reply_.Reset();
  weak_factory_.InvalidateWeakPtrs();
  ui_.reset();
  Detach();
  if (completion_)
    std::move(completion_).Run(std::move(image), std::move(error), action_);
}

void PageCapture::AgentHostClosed(content::DevToolsAgentHost*) {
  attached_ = false;
  Finish();
}
bool PageCapture::MayAttachToURL(const GURL& url, bool is_webui) {
  return !is_webui && url == url_ && Allowed();
}
bool PageCapture::IsTrusted() {
  return false;
}
bool PageCapture::MayReadLocalFiles() {
  return false;
}
bool PageCapture::MayWriteLocalFiles() {
  return false;
}
bool PageCapture::AllowUnsafeOperations() {
  return false;
}
void PageCapture::WebContentsDestroyed() {
  Finish();
}
void PageCapture::PrimaryPageChanged(content::Page&) {
  Finish();
}
void PageCapture::FrameSizeChanged(content::RenderFrameHost* frame,
                                   const gfx::Size&) {
  // A resize invalidates selection coordinates. No resize watcher or repaint
  // loop is retained after capture closes.
  if (!capturing_ && web_contents() &&
      frame == web_contents()->GetPrimaryMainFrame())
    Finish();
}
void PageCapture::OnVisibilityChanged(content::Visibility visibility) {
  if (visibility != content::Visibility::VISIBLE)
    Finish();
}
}  // namespace krypton_ui
