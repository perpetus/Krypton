// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/hosted_browser_widget.h"

#include <set>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/views/frame/browser_native_widget.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "ui/aura/window.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/views/widget/native_widget_aura.h"
#include "ui/views/widget/native_widget_private.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace krypton_ui {
namespace {
class HostedBrowserNativeWidget;
// Live hosted native widgets, so a lookup never downcasts another widget.
std::set<const views::internal::NativeWidgetPrivate*>& HostedWidgets() {
  static base::NoDestructor<
      std::set<const views::internal::NativeWidgetPrivate*>>
      widgets;
  return *widgets;
}

// Window state, activation and visibility belong to the shared frame. The
// coordinator decides which profile is presented; Chromium's own Show and
// Activate requests ask it to present this host instead of moving a window.
class HostedBrowserNativeWidget final : public views::NativeWidgetAura,
                                        public BrowserNativeWidget {
 public:
  HostedBrowserNativeWidget(BrowserWidget* widget, BrowserView* view)
      : views::NativeWidgetAura(widget), view_(view) {
    GetNativeWindow()->SetName("KryptonProfileHost");
    HostedWidgets().insert(this);
  }
  ~HostedBrowserNativeWidget() override { HostedWidgets().erase(this); }
  HostedBrowserNativeWidget(const HostedBrowserNativeWidget&) = delete;
  HostedBrowserNativeWidget& operator=(const HostedBrowserNativeWidget&) =
      delete;

  void SetPresented(bool presented) {
    if (presented) {
      views::NativeWidgetAura::Show(ui::mojom::WindowShowState::kNormal,
                                    gfx::Rect());
    } else {
      SetActive(false);
      views::NativeWidgetAura::Hide();
    }
  }
  void SetActive(bool active) {
    const auto* host = Host();
    if (!host || !host->browser_initialized()) {
      return;
    }
    if (active_ == active) {
      return;
    }
    active_ = active;
    // A child widget never receives OS activation. BrowserView uses this to
    // restore page focus, register extension keybindings and update its
    // accelerators while its profile is the visible one.
    delegate()->OnNativeWidgetActivationChanged(active);
  }

  // BrowserNativeWidget:
  views::Widget::InitParams GetWidgetParams(
      views::Widget::InitParams::Ownership ownership) override {
    views::Widget::InitParams params(ownership);
    params.native_widget = this;
    params.child = true;
    // The frame's DWM material shows through when the shell uses glass.
    params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
    if (auto* frame = Frame()) {
      params.parent = frame->HostParent();
      params.bounds = frame->HostBounds();
    }
    return params;
  }
  bool UseCustomFrame() const override { return true; }
  bool UsesNativeSystemMenu() const override { return false; }
  // Placement belongs to the frame window.
  bool ShouldSaveWindowPlacement() const override { return false; }
  void GetWindowPlacement(
      gfx::Rect* bounds,
      ui::mojom::WindowShowState* show_state) const override {
    *bounds =
        GetWidget() ? GetWidget()->GetWindowBoundsInScreen() : gfx::Rect();
    *show_state = ui::mojom::WindowShowState::kNormal;
  }
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      const input::NativeWebKeyboardEvent&) override {
    return content::KeyboardEventProcessingResult::NOT_HANDLED;
  }
  bool HandleKeyboardEvent(const input::NativeWebKeyboardEvent&) override {
    return false;
  }
  bool ShouldRestorePreviousBrowserWidgetState() const override {
    return false;
  }
  bool ShouldUseInitialVisibleOnAllWorkspaces() const override { return false; }

  // views::NativeWidgetAura:
  void ClientDestroyedWidget() override {
    view_ = nullptr;
    views::NativeWidgetAura::ClientDestroyedWidget();
  }
  void Show(ui::mojom::WindowShowState show_state,
            const gfx::Rect& restore_bounds) override {
    auto* host = Host();
    if (!host || !host->browser_initialized()) {
      return;
    }
    shown_ = true;
    auto* frame = Frame();
    if (host && frame && frame->HostPresented(*host)) {
      views::NativeWidgetAura::Show(ui::mojom::WindowShowState::kNormal,
                                    gfx::Rect());
      return;
    }
    RequestPresentation(show_state != ui::mojom::WindowShowState::kInactive);
  }
  bool shown() const { return shown_; }
  void Activate() override { RequestPresentation(true); }
  void Deactivate() override {}
  bool IsActive() const override {
    auto* host = Host();
    auto* frame = Frame();
    return active_ && host && frame && frame->HostActive(*host);
  }
  void Maximize() override {
    if (auto* widget = FrameWidget()) {
      widget->Maximize();
    }
  }
  void Minimize() override {
    if (auto* widget = FrameWidget()) {
      widget->Minimize();
    }
  }
  bool IsMaximized() const override {
    auto* widget = FrameWidget();
    return widget && widget->IsMaximized();
  }
  bool IsMinimized() const override {
    auto* widget = FrameWidget();
    return widget && widget->IsMinimized();
  }
  void Restore() override {
    if (auto* widget = FrameWidget()) {
      widget->Restore();
    }
  }
  // Page fullscreen fills the shared frame; this host already covers its
  // client area, and its shell follows the frame's window state.
  void SetFullscreen(bool fullscreen, int64_t target_display_id) override {
    if (auto* widget = FrameWidget()) {
      widget->SetFullscreen(fullscreen, target_display_id);
    }
  }
  bool IsFullscreen() const override {
    auto* widget = FrameWidget();
    return widget && widget->IsFullscreen();
  }

 private:
  BrowserAdapter* Host() const { return view_ ? view_->krypton_ui() : nullptr; }
  BrowserAdapter* Frame() const {
    auto* host = Host();
    return host ? host->frame_adapter() : nullptr;
  }
  views::Widget* FrameWidget() const {
    auto* frame = Frame();
    return frame ? frame->FrameWidget() : nullptr;
  }
  void RequestPresentation(bool activate) {
    auto* host = Host();
    auto* frame = Frame();
    if (host && frame) {
      frame->PresentHost(*host, activate);
    }
  }

  raw_ptr<BrowserView> view_;
  bool active_ = false;
  bool shown_ = false;
};

HostedBrowserNativeWidget* FromWidget(views::Widget* widget) {
  auto* native = widget ? widget->native_widget_private() : nullptr;
  return native && HostedWidgets().contains(native)
             ? static_cast<HostedBrowserNativeWidget*>(native)
             : nullptr;
}
}  // namespace

BrowserNativeWidget* CreateHostedBrowserNativeWidget(BrowserWidget* widget,
                                                     BrowserView* view) {
  auto* adapter = view ? view->krypton_ui() : nullptr;
  if (!adapter || !adapter->hosted() || !adapter->frame_adapter()) {
    return nullptr;
  }
  return new HostedBrowserNativeWidget(widget, view);
}

void SetHostedWidgetPresented(views::Widget* widget, bool presented) {
  if (auto* hosted = FromWidget(widget)) {
    hosted->SetPresented(presented);
  }
}

bool HostedWidgetWasShown(views::Widget* widget) {
  const auto* hosted = FromWidget(widget);
  return hosted && hosted->shown();
}

void SetHostedWidgetActive(views::Widget* widget, bool active) {
  if (auto* hosted = FromWidget(widget)) {
    hosted->SetActive(active);
  }
}
}  // namespace krypton_ui
