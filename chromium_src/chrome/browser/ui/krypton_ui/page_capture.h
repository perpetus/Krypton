// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_PAGE_CAPTURE_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_PAGE_CAPTURE_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/image/image.h"
#include "url/gurl.h"

class SkBitmap;
namespace tabs {
class TabInterface;
}

namespace krypton_ui {
// kArea and kElement open the same overlay: drag selects a region and a click
// selects the hovered element. kFullPage captures immediately.
enum class PageCaptureMode { kArea, kElement, kFullPage };
// What the owner does with a finished image.
enum class PageCaptureAction { kPreview, kCopy, kDownload, kEasel };
struct PageCaptureOptions {
  bool add_to_easel = false;
  bool dark = false;
};
class PageCaptureUI;

// Exists only for an explicit capture. No injected scripts, permanent observers
// or page instrumentation. Element hover and full-page capture use a transient
// inspector session that is detached when the capture ends. The owner receives
// completion after callbacks unwind.
class PageCapture final : public content::WebContentsObserver,
                          public content::DevToolsAgentHostClient {
 public:
  using Completion =
      base::OnceCallback<void(gfx::Image, std::string, PageCaptureAction)>;
  explicit PageCapture(tabs::TabInterface& tab);
  ~PageCapture() override;
  void Start(PageCaptureMode mode,
             PageCaptureOptions options,
             Completion completion);

 private:
  using Reply = base::OnceCallback<void(const base::DictValue&)>;
  // Receives null when the inspector reports an error.
  using HoverReply = base::OnceCallback<void(const base::DictValue*)>;
  bool Attach(std::string* error);
  void ReadViewport(const base::DictValue& result);
  void HoverAt(gfx::Point point);
  void QueryHover();
  void HoverNode(const base::DictValue* result);
  void HoverBox(const base::DictValue* result);
  void HoverDone(std::optional<gfx::Rect> bounds);
  void CaptureRegion(gfx::Rect bounds, PageCaptureAction action);
  void CaptureFullPage();
  void Send(std::string_view method, base::DictValue params, Reply reply);
  void SendHover(std::string_view method,
                 base::DictValue params,
                 HoverReply reply);
  void ReadMetrics(const base::DictValue& result);
  void Capture(gfx::RectF bounds, double zoom);
  void Decoded(SkBitmap bitmap);
  void Finish(gfx::Image image = {}, std::string error = {});
  void Detach();
  bool Allowed() const;

  void DispatchProtocolMessage(content::DevToolsAgentHost* host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* host) override;
  bool MayAttachToURL(const GURL& url, bool is_webui) override;
  bool IsTrusted() override;
  bool MayReadLocalFiles() override;
  bool MayWriteLocalFiles() override;
  bool AllowUnsafeOperations() override;
  void WebContentsDestroyed() override;
  void PrimaryPageChanged(content::Page& page) override;
  void FrameSizeChanged(content::RenderFrameHost* frame,
                        const gfx::Size& size) override;
  void OnVisibilityChanged(content::Visibility visibility) override;

  const GURL url_;
  Completion completion_;
  PageCaptureOptions options_;
  PageCaptureAction action_ = PageCaptureAction::kPreview;
  std::unique_ptr<PageCaptureUI> ui_;
  scoped_refptr<content::DevToolsAgentHost> agent_;
  Reply reply_;
  HoverReply hover_reply_;
  int next_id_ = 0;
  int pending_id_ = 0;
  int hover_id_ = 0;
  bool attached_ = false;
  bool capturing_ = false;
  bool finished_ = false;
  bool preferences_saved_ = false;
  bool original_hide_scrollbars_ = false;
  bool original_record_whole_document_ = false;
  // Visual viewport origin in document CSS pixels, and CSS-to-DIP scale. Set
  // once per overlay; the overlay blocks scrolling while it is open.
  bool viewport_ready_ = false;
  double page_x_ = 0;
  double page_y_ = 0;
  double css_to_dip_ = 1;
  // At most one hover query is in flight; the latest pointer position wins.
  std::optional<gfx::Point> hover_point_;
  gfx::Point hover_sent_;
  // A one-shot deadline for an outstanding request, never a state watcher.
  base::OneShotTimer deadline_;
  base::CallbackListSubscription deactivated_;
  base::CallbackListSubscription detached_;
  base::WeakPtrFactory<PageCapture> weak_factory_{this};
};
}  // namespace krypton_ui
#endif
