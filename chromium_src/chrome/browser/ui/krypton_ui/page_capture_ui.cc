// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/page_capture_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/notreached.h"
#include "base/scoped_observation.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/path_effect.h"
#include "chrome/browser/ui/krypton_ui/page_capture_icons.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkMatrix.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/aura/window.h"
#include "ui/aura/window_observer.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace krypton_ui {
namespace {
using capture_icons::Shape;

// Values from Zen's overlay: a half-dimmed page, a white 20% hover fill with a
// 2px white 40% outline and 6px corners, and a dashed white selection.
constexpr SkColor kScrim = SkColorSetARGB(0x80, 0, 0, 0);
constexpr SkColor kHoverFill = SkColorSetARGB(0x33, 0xFF, 0xFF, 0xFF);
constexpr SkColor kHoverOutline = SkColorSetARGB(0x66, 0xFF, 0xFF, 0xFF);
constexpr SkColor kSelectionOutline = SkColorSetARGB(0xCC, 0xFF, 0xFF, 0xFF);
constexpr float kHoverRadius = 6;
constexpr auto kHoverTransition = base::Milliseconds(100);
constexpr int kFaceSize = 64;
constexpr int kInstructionsWidth = 400;
constexpr int kInstructionsPadding = 20;
constexpr int kInstructionsFontSize = 24;
constexpr int kCancelGap = 20;
constexpr int kPanelInset = 8;
constexpr int kPanelPadding = 8;
constexpr int kPanelGap = 4;
constexpr float kPanelRadius = 8;
constexpr int kPageButtonWidth = 90;
constexpr int kPageButtonBottom = 5;
constexpr int kActionsGap = 10;
constexpr int kActionsPadding = 4;
constexpr int kActionHeight = 32;
constexpr float kControlRadius = 4;
constexpr int kDragThreshold = 4;
constexpr int kMinSelection = 4;
constexpr int kCornerHit = 16;
constexpr int kEdgeHit = 10;
constexpr std::u16string_view kInstructions =
    u"Drag or click on the page to select a region. Press ESC to cancel.";

struct Palette {
  SkColor panel;
  SkColor button;
  SkColor button_hover;
  SkColor text;
  SkColor hover_text;
  SkColor accent;
  SkColor accent_text;
};
// The dark set is measured from Zen's dark panel; the light set follows its
// light panel. The page itself is always dimmed, so the face stays white.
constexpr Palette kDarkPalette{
    .panel = SkColorSetRGB(0x42, 0x41, 0x4D),
    .button = SkColorSetRGB(0x55, 0x54, 0x5F),
    .button_hover = SkColorSetRGB(0x8A, 0xA1, 0xCD),
    .text = SkColorSetRGB(0xFB, 0xFB, 0xFE),
    .hover_text = SkColorSetRGB(0x15, 0x14, 0x1A),
    .accent = SkColorSetRGB(0x8A, 0xA1, 0xCD),
    .accent_text = SkColorSetRGB(0x15, 0x14, 0x1A),
};
constexpr Palette kLightPalette{
    .panel = SkColorSetRGB(0xF9, 0xF9, 0xFB),
    .button = SkColorSetRGB(0xE9, 0xE9, 0xED),
    .button_hover = SkColorSetRGB(0xD0, 0xD0, 0xD7),
    .text = SkColorSetRGB(0x15, 0x14, 0x1A),
    .hover_text = SkColorSetRGB(0x15, 0x14, 0x1A),
    .accent = SkColorSetRGB(0x00, 0x61, 0xE0),
    .accent_text = SK_ColorWHITE,
};
// The secondary fill inside the page icons.
constexpr SkColor kIconViewport = SkColorSetRGB(0x8F, 0x8F, 0x9D);

const Palette& PaletteFor(bool dark) {
  return dark ? kDarkPalette : kLightPalette;
}

void DrawShape(gfx::Canvas* canvas,
               Shape shape,
               float source_size,
               const gfx::RectF& target,
               SkColor color) {
  SkMatrix matrix = SkMatrix::Scale(target.width() / source_size,
                                    target.height() / source_size);
  matrix.postTranslate(target.x(), target.y());
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(color);
  canvas->DrawPath(capture_icons::Path(shape).makeTransform(matrix), flags);
}

SkColor HalfAlpha(SkColor color) {
  return SkColorSetA(color, SkColorGetA(color) / 2);
}

enum class ButtonKind { kFullPage, kVisible, kGhost, kClose, kCopy, kPrimary };
}  // namespace

// Custom-painted so the controls match the reference overlay exactly while
// keeping Views focus, hover, tooltips and accessibility.
class CaptureButton final : public views::Button {
  METADATA_HEADER(CaptureButton, views::Button)
 public:
  CaptureButton(PressedCallback callback,
                ButtonKind kind,
                std::u16string label,
                std::u16string tooltip,
                bool dark)
      : Button(std::move(callback)),
        kind_(kind),
        label_(std::move(label)),
        dark_(dark),
        font_(kind == ButtonKind::kFullPage || kind == ButtonKind::kVisible
                  ? gfx::FontList().DeriveWithSizeDelta(1)
                  : gfx::FontList()) {
    // Clicking must not move keyboard focus out of the page.
    SetRequestFocusOnPress(false);
    SetTooltipText(tooltip);
    GetViewAccessibility().SetName(label_.empty() ? tooltip : label_);
    views::InstallRoundRectHighlightPathGenerator(
        this, gfx::Insets(), static_cast<int>(kControlRadius));
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    const int text = gfx::GetStringWidth(label_, font_);
    switch (kind_) {
      case ButtonKind::kFullPage:
      case ButtonKind::kVisible:
        return gfx::Size(std::max(kPageButtonWidth, text + 10),
                         static_cast<int>(capture_icons::kPageIconSize) +
                             font_.GetHeight() + kPageButtonBottom);
      case ButtonKind::kGhost:
        return gfx::Size(text + 32, kActionHeight);
      case ButtonKind::kClose:
        return gfx::Size(kActionHeight, kActionHeight);
      case ButtonKind::kCopy:
      case ButtonKind::kPrimary:
        return gfx::Size(text + 16 + 4 + 24, kActionHeight);
    }
    NOTREACHED();
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const Palette& palette = PaletteFor(dark_);
    const auto state = GetState();
    const bool active = state == STATE_HOVERED || state == STATE_PRESSED;
    const gfx::RectF bounds(GetLocalBounds());
    canvas->SaveLayerAlpha(GetEnabled() ? 0xFF : 0x66);
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    SkColor foreground = palette.text;
    if (kind_ == ButtonKind::kGhost) {
      // A white outline on the dimmed page that fills white on hover.
      if (active) {
        fill.setColor(SK_ColorWHITE);
        canvas->DrawRoundRect(bounds, kControlRadius, fill);
        foreground = SK_ColorBLACK;
      } else {
        fill.setStyle(cc::PaintFlags::kStroke_Style);
        fill.setStrokeWidth(1);
        fill.setColor(SK_ColorWHITE);
        gfx::RectF outline = bounds;
        outline.Inset(0.5f);
        canvas->DrawRoundRect(outline, kControlRadius, fill);
        foreground = SK_ColorWHITE;
      }
    } else {
      SkColor background = palette.button;
      if (kind_ == ButtonKind::kPrimary) {
        background = palette.accent;
        foreground = palette.accent_text;
      } else if (active) {
        background = palette.button_hover;
        foreground = palette.hover_text;
      }
      if (state == STATE_PRESSED)
        background = color_utils::AlphaBlend(SK_ColorBLACK, background, 0.12f);
      else if (kind_ == ButtonKind::kPrimary && active)
        background = color_utils::AlphaBlend(SK_ColorWHITE, background, 0.12f);
      fill.setColor(background);
      canvas->DrawRoundRect(bounds, kControlRadius, fill);
    }

    const float glyph = capture_icons::kGlyphSize;
    switch (kind_) {
      case ButtonKind::kFullPage:
      case ButtonKind::kVisible: {
        const float size = capture_icons::kPageIconSize;
        const gfx::RectF icon((bounds.width() - size) / 2, 0, size, size);
        if (kind_ == ButtonKind::kFullPage) {
          DrawShape(canvas, Shape::kFullPageViewport, size, icon,
                    kIconViewport);
          DrawShape(canvas, Shape::kFullPageFrame, size, icon, foreground);
          DrawShape(canvas, Shape::kFullPageFrameTop, size, icon, foreground);
          DrawShape(canvas, Shape::kFullPageFrameBottom, size, icon,
                    foreground);
          DrawShape(canvas, Shape::kFullPageDash, size, icon,
                    HalfAlpha(foreground));
        } else {
          DrawShape(canvas, Shape::kVisibleFrame, size, icon, foreground);
          DrawShape(canvas, Shape::kVisibleViewport, size, icon, kIconViewport);
          DrawShape(canvas, Shape::kVisibleDash, size, icon,
                    HalfAlpha(foreground));
        }
        canvas->DrawStringRectWithFlags(
            label_, font_, foreground,
            gfx::Rect(0, static_cast<int>(size), width(),
                      height() - static_cast<int>(size) - kPageButtonBottom),
            gfx::Canvas::TEXT_ALIGN_CENTER);
        break;
      }
      case ButtonKind::kGhost:
        canvas->DrawStringRectWithFlags(label_, font_, foreground,
                                        GetLocalBounds(),
                                        gfx::Canvas::TEXT_ALIGN_CENTER);
        break;
      case ButtonKind::kClose:
        DrawShape(canvas, Shape::kClose, glyph,
                  gfx::RectF((bounds.width() - glyph) / 2,
                             (bounds.height() - glyph) / 2, glyph, glyph),
                  foreground);
        break;
      case ButtonKind::kCopy:
      case ButtonKind::kPrimary: {
        const int text = gfx::GetStringWidth(label_, font_);
        const float left = (bounds.width() - (glyph + 4 + text)) / 2;
        const gfx::RectF icon(left, (bounds.height() - glyph) / 2, glyph,
                              glyph);
        if (kind_ == ButtonKind::kCopy) {
          DrawShape(canvas, Shape::kCopy, glyph, icon, foreground);
          DrawShape(canvas, Shape::kCopyBody, glyph, icon, foreground);
        } else {
          DrawShape(canvas, Shape::kDownload, glyph, icon, foreground);
        }
        canvas->DrawStringRectWithFlags(
            label_, font_, foreground,
            gfx::Rect(static_cast<int>(left + glyph + 4), 0, text + 1,
                      height()),
            gfx::Canvas::TEXT_ALIGN_LEFT);
        break;
      }
    }
    canvas->Restore();
  }

 private:
  const ButtonKind kind_;
  const std::u16string label_;
  const bool dark_;
  const gfx::FontList font_;
};
BEGIN_METADATA(CaptureButton)
END_METADATA

class CaptureOverlayView final : public views::View,
                                 public views::AnimationDelegateViews {
  METADATA_HEADER(CaptureOverlayView, views::View)
 public:
  CaptureOverlayView(
      PageCaptureOptions options,
      base::RepeatingCallback<void(gfx::Point)> hover,
      base::RepeatingCallback<void(gfx::Rect, PageCaptureAction)> region,
      base::RepeatingClosure full_page,
      base::RepeatingClosure cancel)
      : AnimationDelegateViews(this),
        options_(options),
        hover_(std::move(hover)),
        region_(std::move(region)),
        full_page_(std::move(full_page)),
        cancel_(std::move(cancel)),
        instructions_font_(gfx::FontList().DeriveWithSizeDelta(
            kInstructionsFontSize - gfx::FontList().GetFontSize())) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    GetViewAccessibility().SetRole(ax::mojom::Role::kPane);
    GetViewAccessibility().SetName(std::u16string(kInstructions));
    const bool easel = options_.add_to_easel;
    const bool dark = options_.dark;
    full_page_button_ = AddChildView(std::make_unique<CaptureButton>(
        base::BindRepeating(&CaptureOverlayView::FullPage,
                            base::Unretained(this)),
        ButtonKind::kFullPage, easel ? u"Add full page" : u"Save full page",
        easel ? u"Add the full page to Easel" : u"Save full page", dark));
    visible_button_ = AddChildView(std::make_unique<CaptureButton>(
        base::BindRepeating(&CaptureOverlayView::Visible,
                            base::Unretained(this)),
        ButtonKind::kVisible, easel ? u"Add visible" : u"Save visible",
        easel ? u"Add the visible page to Easel" : u"Save visible", dark));
    ghost_cancel_ = AddChildView(std::make_unique<CaptureButton>(
        cancel_, ButtonKind::kGhost, u"Cancel", u"Cancel (Esc)", dark));
    close_ = AddChildView(std::make_unique<CaptureButton>(
        cancel_, ButtonKind::kClose, std::u16string(), u"Cancel (Esc)", dark));
    if (!easel) {
      copy_ = AddChildView(std::make_unique<CaptureButton>(
          base::BindRepeating(&CaptureOverlayView::Run, base::Unretained(this),
                              PageCaptureAction::kCopy),
          ButtonKind::kCopy, u"Copy", u"Copy (Ctrl+C)", dark));
    }
    primary_ = AddChildView(std::make_unique<CaptureButton>(
        base::BindRepeating(&CaptureOverlayView::Run, base::Unretained(this),
                            Primary()),
        ButtonKind::kPrimary, easel ? u"Add to Easel" : u"Download",
        easel ? u"Add to Easel (Enter)" : u"Download (Ctrl+S)", dark));
  }

  void SetHover(std::optional<gfx::Rect> bounds) {
    if (hidden_ || state_ != State::kCrosshairs || !pointer_inside_)
      bounds.reset();
    hover_target_ = bounds;
    if (!bounds) {
      if (std::exchange(hover_visible_, false)) {
        hover_animation_.Stop();
        SchedulePaint();
      }
      return;
    }
    const gfx::RectF target(*bounds);
    if (!hover_visible_) {
      hover_visible_ = true;
      hover_shown_ = hover_to_ = target;
      SchedulePaint();
      return;
    }
    if (target == hover_to_)
      return;
    hover_from_ = hover_shown_;
    hover_to_ = target;
    hover_animation_.Start();
  }

  void SetFullPageUnavailable(const std::u16string& reason) {
    full_page_button_->SetEnabled(false);
    full_page_button_->SetTooltipText(reason);
  }

  void HideOverlay() {
    hidden_ = true;
    pressed_ = false;
    hover_animation_.Stop();
    hover_visible_ = false;
    UpdateChildren();
  }

  // Keys arrive from the page's window, so focus never leaves the page.
  bool HandleKey(const ui::KeyEvent& event) {
    if (event.key_code() == ui::VKEY_ESCAPE) {
      cancel_.Run();
      return true;
    }
    if (hidden_ || state_ != State::kSelected)
      return false;
    // A focused overlay button handles its own activation keys.
    if (const auto* focus = GetFocusManager();
        focus && Contains(focus->GetFocusedView()))
      return false;
    if (event.key_code() == ui::VKEY_RETURN ||
        (event.IsControlDown() && event.key_code() == ui::VKEY_S)) {
      Run(Primary());
      return true;
    }
    if (copy_ && event.IsControlDown() && event.key_code() == ui::VKEY_C) {
      Run(PageCaptureAction::kCopy);
      return true;
    }
    return false;
  }

  // views::View:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    UpdateChildren();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    if (hidden_)
      return;
    const gfx::Rect bounds = GetLocalBounds();
    if (state_ == State::kCrosshairs) {
      canvas->FillRect(bounds, kScrim);
      PaintFace(canvas);
      canvas->DrawStringRectWithFlags(
          kInstructions, instructions_font_, SK_ColorWHITE, text_,
          gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::MULTI_LINE);
      if (hover_visible_)
        PaintHover(canvas);
      PaintPanel(canvas);
      return;
    }
    canvas->Save();
    canvas->ClipPath(SkPath::Rect(gfx::RectToSkRect(selection_)), false,
                     SkClipOp::kDifference);
    canvas->FillRect(bounds, kScrim);
    canvas->Restore();
    PaintSelection(canvas);
    if (!actions_.IsEmpty()) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(PaletteFor(options_.dark).panel);
      canvas->DrawRoundRect(gfx::RectF(actions_), kControlRadius, flags);
    }
  }

  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    using ui::mojom::CursorType;
    if (state_ != State::kSelected)
      return ui::Cursor(CursorType::kCross);
    switch (pressed_ ? grip_ : GripAt(event.location())) {
      case Grip::kMove:
        return ui::Cursor(pressed_ ? CursorType::kGrabbing : CursorType::kMove);
      case Grip::kTopLeft:
      case Grip::kBottomRight:
        return ui::Cursor(CursorType::kNorthWestSouthEastResize);
      case Grip::kTopRight:
      case Grip::kBottomLeft:
        return ui::Cursor(CursorType::kNorthEastSouthWestResize);
      case Grip::kTop:
      case Grip::kBottom:
        return ui::Cursor(CursorType::kNorthSouthResize);
      case Grip::kLeft:
      case Grip::kRight:
        return ui::Cursor(CursorType::kEastWestResize);
      case Grip::kNone:
        return ui::Cursor(CursorType::kCross);
    }
    NOTREACHED();
  }

  void OnMouseEntered(const ui::MouseEvent& event) override {
    pointer_inside_ = true;
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    pointer_inside_ = false;
    SetHover(std::nullopt);
  }
  void OnMouseMoved(const ui::MouseEvent& event) override {
    pointer_inside_ = true;
    if (hidden_ || state_ != State::kCrosshairs)
      return;
    UpdateEyes(event.location());
    if (!panel_.Contains(event.location()))
      hover_.Run(event.location());
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (hidden_ || !event.IsOnlyLeftMouseButton())
      return false;
    const gfx::Point point = Clamp(event.location());
    // Swallow presses in the panel's and action row's padding.
    if ((state_ == State::kCrosshairs && panel_.Contains(point)) ||
        actions_.Contains(point))
      return true;
    pressed_ = true;
    moved_ = false;
    press_ = point;
    origin_ = selection_;
    grip_ = state_ == State::kSelected ? GripAt(point) : Grip::kNone;
    if (grip_ != Grip::kNone)
      LayoutActions();
    return true;
  }

  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!pressed_)
      return true;
    const gfx::Point point = Clamp(event.location());
    if (!moved_ && std::abs(point.x() - press_.x()) < kDragThreshold &&
        std::abs(point.y() - press_.y()) < kDragThreshold)
      return true;
    if (!std::exchange(moved_, true) && grip_ == Grip::kNone) {
      // A drag outside the selection starts a new one.
      state_ = State::kDragging;
      SetHover(std::nullopt);
      UpdateChildren();
    }
    selection_ =
        grip_ == Grip::kNone ? Between(press_, point) : Adjusted(point);
    SchedulePaint();
    return true;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (!std::exchange(pressed_, false))
      return;
    if (moved_) {
      Settle();
    } else if (state_ == State::kCrosshairs) {
      // A click selects the highlighted element.
      if (hover_target_)
        Select(*hover_target_);
    } else if (state_ == State::kSelected && grip_ == Grip::kNone) {
      // A click outside the selection starts over.
      Reset();
    } else {
      LayoutActions();
    }
  }

  void OnMouseCaptureLost() override {
    if (!std::exchange(pressed_, false))
      return;
    if (moved_)
      Settle();
    else
      LayoutActions();
  }

  // Scrolling would move the page under a fixed selection.
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override { return true; }

  // views::AnimationDelegateViews:
  void AnimationProgressed(const gfx::Animation* animation) override {
    hover_shown_ = gfx::Tween::RectFValueBetween(
        gfx::Tween::CalculateValue(gfx::Tween::EASE_OUT,
                                   animation->GetCurrentValue()),
        hover_from_, hover_to_);
    SchedulePaint();
  }

 private:
  enum class State { kCrosshairs, kDragging, kSelected };
  enum class Grip {
    kNone,
    kMove,
    kTopLeft,
    kTop,
    kTopRight,
    kLeft,
    kRight,
    kBottomLeft,
    kBottom,
    kBottomRight,
  };

  PageCaptureAction Primary() const {
    return options_.add_to_easel ? PageCaptureAction::kEasel
                                 : PageCaptureAction::kDownload;
  }
  void Run(PageCaptureAction action) {
    if (!hidden_ && state_ == State::kSelected && !selection_.IsEmpty())
      region_.Run(selection_, action);
  }
  void FullPage() {
    if (!hidden_)
      full_page_.Run();
  }
  void Visible() {
    if (!hidden_)
      region_.Run(GetLocalBounds(), options_.add_to_easel
                                        ? PageCaptureAction::kEasel
                                        : PageCaptureAction::kPreview);
  }

  gfx::Point Clamp(gfx::Point point) const {
    point.SetToMax(gfx::Point());
    point.SetToMin(gfx::Point(width(), height()));
    return point;
  }
  gfx::Rect Between(gfx::Point a, gfx::Point b) const {
    gfx::Rect rect = gfx::BoundingRect(a, b);
    rect.Intersect(GetLocalBounds());
    return rect;
  }
  gfx::Rect Adjusted(gfx::Point point) const {
    const gfx::Vector2d delta = point - press_;
    if (grip_ == Grip::kMove) {
      gfx::Rect moved = origin_;
      moved.Offset(delta);
      moved.AdjustToFit(GetLocalBounds());
      return moved;
    }
    int left = origin_.x(), top = origin_.y();
    int right = origin_.right(), bottom = origin_.bottom();
    if (grip_ == Grip::kTopLeft || grip_ == Grip::kLeft ||
        grip_ == Grip::kBottomLeft)
      left += delta.x();
    if (grip_ == Grip::kTopRight || grip_ == Grip::kRight ||
        grip_ == Grip::kBottomRight)
      right += delta.x();
    if (grip_ == Grip::kTopLeft || grip_ == Grip::kTop ||
        grip_ == Grip::kTopRight)
      top += delta.y();
    if (grip_ == Grip::kBottomLeft || grip_ == Grip::kBottom ||
        grip_ == Grip::kBottomRight)
      bottom += delta.y();
    return Between(Clamp(gfx::Point(left, top)),
                   Clamp(gfx::Point(right, bottom)));
  }
  Grip GripAt(gfx::Point point) const {
    const gfx::Rect& r = selection_;
    const auto close_to = [](int value, int edge, int reach) {
      return std::abs(value - edge) <= reach;
    };
    const bool left = close_to(point.x(), r.x(), kCornerHit);
    const bool right = close_to(point.x(), r.right(), kCornerHit);
    const bool top = close_to(point.y(), r.y(), kCornerHit);
    const bool bottom = close_to(point.y(), r.bottom(), kCornerHit);
    if (top && left)
      return Grip::kTopLeft;
    if (top && right)
      return Grip::kTopRight;
    if (bottom && left)
      return Grip::kBottomLeft;
    if (bottom && right)
      return Grip::kBottomRight;
    const bool across = point.x() >= r.x() && point.x() <= r.right();
    const bool down = point.y() >= r.y() && point.y() <= r.bottom();
    if (across && close_to(point.y(), r.y(), kEdgeHit))
      return Grip::kTop;
    if (across && close_to(point.y(), r.bottom(), kEdgeHit))
      return Grip::kBottom;
    if (down && close_to(point.x(), r.x(), kEdgeHit))
      return Grip::kLeft;
    if (down && close_to(point.x(), r.right(), kEdgeHit))
      return Grip::kRight;
    return r.Contains(point) ? Grip::kMove : Grip::kNone;
  }

  void Settle() {
    if (selection_.width() >= kMinSelection &&
        selection_.height() >= kMinSelection)
      Select(selection_);
    else
      Reset();
  }
  void Select(const gfx::Rect& selection) {
    selection_ = selection;
    state_ = State::kSelected;
    SetHover(std::nullopt);
    UpdateChildren();
    SchedulePaint();
  }
  void Reset() {
    selection_ = {};
    state_ = State::kCrosshairs;
    UpdateChildren();
    SchedulePaint();
  }

  // Pupils lean toward the pointer, as in the reference face.
  void UpdateEyes(gfx::Point point) {
    const double w = std::max(1, width()), h = std::max(1, height());
    const gfx::Vector2d eyes(
        static_cast<int>(std::floor(10 * (point.x() - w / 2) / w)),
        static_cast<int>(std::floor(10 * (point.y() - h / 2) / h)));
    if (eyes == eyes_)
      return;
    eyes_ = eyes;
    SchedulePaintInRect(face_);
  }

  void UpdateChildren() {
    const gfx::Rect bounds = GetLocalBounds();
    const bool crosshairs = !hidden_ && state_ == State::kCrosshairs;
    const gfx::Size full = full_page_button_->GetPreferredSize();
    const gfx::Size visible = visible_button_->GetPreferredSize();
    const int button_height = std::max(full.height(), visible.height());
    const int panel_width =
        kPanelPadding * 2 + full.width() + kPanelGap + visible.width();
    panel_ = gfx::Rect(bounds.right() - kPanelInset - panel_width, kPanelInset,
                       panel_width, kPanelPadding * 2 + button_height);
    full_page_button_->SetBounds(panel_.x() + kPanelPadding,
                                 panel_.y() + kPanelPadding, full.width(),
                                 button_height);
    visible_button_->SetBounds(full_page_button_->bounds().right() + kPanelGap,
                               panel_.y() + kPanelPadding, visible.width(),
                               button_height);
    full_page_button_->SetVisible(crosshairs);
    visible_button_->SetVisible(crosshairs);

    const int text_width = std::clamp(bounds.width() - kInstructionsPadding * 2,
                                      0, kInstructionsWidth);
    int measured_width = text_width, text_height = 0;
    gfx::Canvas::SizeStringInt(
        kInstructions, instructions_font_, &measured_width, &text_height, 0,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::MULTI_LINE);
    const gfx::Size cancel = ghost_cancel_->GetPreferredSize();
    const int center = bounds.CenterPoint().x();
    int y = (bounds.height() - (kFaceSize + kInstructionsPadding * 2 +
                                text_height + kCancelGap + cancel.height())) /
            2;
    face_ = gfx::Rect(center - kFaceSize / 2, y, kFaceSize, kFaceSize);
    y += kFaceSize + kInstructionsPadding;
    text_ = gfx::Rect(center - text_width / 2, y, text_width, text_height);
    y += text_height + kInstructionsPadding + kCancelGap;
    ghost_cancel_->SetBounds(center - cancel.width() / 2, y, cancel.width(),
                             cancel.height());
    ghost_cancel_->SetVisible(crosshairs);
    LayoutActions();
  }

  // Places Cancel, Copy and Download below the selection, or above or inside
  // it when there is no room, matching the reference overlay.
  void LayoutActions() {
    std::array<CaptureButton*, 3> buttons = {close_, copy_, primary_};
    const bool show = !hidden_ && state_ == State::kSelected && !pressed_;
    int content = 0;
    for (auto* button : buttons) {
      if (!button)
        continue;
      button->SetVisible(show);
      if (show)
        content += button->GetPreferredSize().width();
    }
    if (!show) {
      actions_ = {};
      return;
    }
    content += kActionsPadding * (copy_ ? 2 : 1);
    const gfx::Size size(content + kActionsPadding * 2,
                         kActionHeight + kActionsPadding * 2);
    const int x = std::clamp(selection_.right() - size.width(), 0,
                             std::max(0, width() - size.width()));
    int y = selection_.bottom() + kActionsGap;
    if (y + size.height() > height()) {
      y = selection_.y() - kActionsGap - size.height();
      if (y < 0)
        y = std::max(0, selection_.bottom() - kActionsGap - size.height());
    }
    actions_ = gfx::Rect(gfx::Point(x, y), size);
    int left = x + kActionsPadding;
    for (auto* button : buttons) {
      if (!button)
        continue;
      const int button_width = button->GetPreferredSize().width();
      button->SetBounds(left, y + kActionsPadding, button_width, kActionHeight);
      left += button_width + kActionsPadding;
    }
    SchedulePaint();
  }

  void PaintFace(gfx::Canvas* canvas) {
    const gfx::RectF face(face_);
    DrawShape(canvas, Shape::kFaceLines, capture_icons::kFaceSize, face,
              SK_ColorWHITE);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(SK_ColorWHITE);
    for (const float x : {23.f, 43.f})
      canvas->sk_canvas()->drawOval(
          SkRect::MakeXYWH(face.x() + x - 5, face.y() + 19, 10, 14), flags);
    flags.setColor(SK_ColorBLACK);
    for (const float x : {25.f, 45.f})
      canvas->DrawCircle(
          gfx::PointF(face.x() + x + eyes_.x(), face.y() + 30 + eyes_.y()), 3,
          flags);
  }

  void PaintHover(gfx::Canvas* canvas) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(kHoverFill);
    canvas->DrawRoundRect(hover_shown_, kHoverRadius, flags);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(2);
    flags.setColor(kHoverOutline);
    gfx::RectF outline = hover_shown_;
    outline.Inset(1);
    canvas->DrawRoundRect(outline, kHoverRadius - 1, flags);
  }

  void PaintPanel(gfx::Canvas* canvas) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(PaletteFor(options_.dark).panel);
    canvas->DrawRoundRect(gfx::RectF(panel_), kPanelRadius, flags);
  }

  void PaintSelection(gfx::Canvas* canvas) {
    if (selection_.IsEmpty())
      return;
    cc::PaintFlags outline;
    outline.setStyle(cc::PaintFlags::kStroke_Style);
    outline.setStrokeWidth(2);
    outline.setColor(kSelectionOutline);
    constexpr float kDash[] = {6, 4};
    outline.setPathEffect(cc::PathEffect::MakeDash(kDash, 2, 0));
    gfx::RectF border(selection_);
    border.Inset(1);
    canvas->DrawRect(border, outline);

    // Output size in physical pixels, centred while it fits.
    const float scale = layer()->device_scale_factor();
    const std::u16string size = base::StrCat(
        {base::NumberToString16(
             static_cast<int>(std::lround(selection_.width() * scale))),
         u" \u00D7 ",
         base::NumberToString16(
             static_cast<int>(std::lround(selection_.height() * scale)))});
    const gfx::FontList font;
    const gfx::Size text(gfx::GetStringWidth(size, font), font.GetHeight());
    gfx::Rect label(text);
    label.Outset(4);
    if (label.width() + 8 <= selection_.width() &&
        label.height() + 8 <= selection_.height()) {
      label.set_origin(selection_.CenterPoint() -
                       gfx::Vector2d(label.width() / 2, label.height() / 2));
      const Palette& palette = PaletteFor(options_.dark);
      cc::PaintFlags fill;
      fill.setAntiAlias(true);
      fill.setColor(palette.panel);
      canvas->DrawRoundRect(gfx::RectF(label), kControlRadius, fill);
      canvas->DrawStringRectWithFlags(size, font, palette.text, label,
                                      gfx::Canvas::TEXT_ALIGN_CENTER);
    }

    if (state_ != State::kSelected)
      return;
    const bool compact = selection_.width() < 60 || selection_.height() < 60;
    const float radius = compact ? 5 : 8;
    const gfx::RectF r(selection_);
    cc::PaintFlags shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(SkColorSetARGB(0x55, 0, 0, 0));
    cc::PaintFlags mover;
    mover.setAntiAlias(true);
    mover.setColor(SK_ColorWHITE);
    for (const gfx::PointF& point :
         {r.origin(), r.top_center(), r.top_right(), r.left_center(),
          r.right_center(), r.bottom_left(), r.bottom_center(),
          r.bottom_right()}) {
      canvas->DrawCircle(point, radius + 1.5f, shadow);
      canvas->DrawCircle(point, radius, mover);
    }
  }

  const PageCaptureOptions options_;
  base::RepeatingCallback<void(gfx::Point)> hover_;
  base::RepeatingCallback<void(gfx::Rect, PageCaptureAction)> region_;
  base::RepeatingClosure full_page_;
  base::RepeatingClosure cancel_;
  const gfx::FontList instructions_font_;
  raw_ptr<CaptureButton> full_page_button_ = nullptr;
  raw_ptr<CaptureButton> visible_button_ = nullptr;
  raw_ptr<CaptureButton> ghost_cancel_ = nullptr;
  raw_ptr<CaptureButton> close_ = nullptr;
  raw_ptr<CaptureButton> copy_ = nullptr;
  raw_ptr<CaptureButton> primary_ = nullptr;
  State state_ = State::kCrosshairs;
  Grip grip_ = Grip::kNone;
  bool hidden_ = false;
  bool pressed_ = false;
  bool moved_ = false;
  bool pointer_inside_ = false;
  gfx::Point press_;
  gfx::Rect origin_;
  gfx::Rect selection_;
  gfx::Rect panel_;
  gfx::Rect actions_;
  gfx::Rect face_;
  gfx::Rect text_;
  gfx::Vector2d eyes_;
  std::optional<gfx::Rect> hover_target_;
  bool hover_visible_ = false;
  gfx::RectF hover_shown_;
  gfx::RectF hover_from_;
  gfx::RectF hover_to_;
  // Runs only for the 100 ms transition after a hover change.
  gfx::LinearAnimation hover_animation_{
      kHoverTransition, gfx::LinearAnimation::kDefaultFrameRate, this};
};
BEGIN_METADATA(CaptureOverlayView)
END_METADATA

// Handles Esc and the selection shortcuts before the page sees them. Registered
// on the page's window so keyboard focus never moves into the overlay.
class CaptureKeyHandler final : public ui::EventHandler,
                                public aura::WindowObserver {
 public:
  CaptureKeyHandler(aura::Window* window, CaptureOverlayView* overlay)
      : window_(window), overlay_(overlay) {
    window_->AddPreTargetHandler(this);
    observation_.Observe(window_);
  }
  ~CaptureKeyHandler() override {
    if (window_)
      window_->RemovePreTargetHandler(this);
  }

  void OnKeyEvent(ui::KeyEvent* event) override {
    if (event->type() != ui::EventType::kKeyPressed)
      return;
    // The character event that follows a handled key must not reach the page.
    if (event->is_char()) {
      if (std::exchange(swallow_char_, false))
        event->StopPropagation();
      return;
    }
    swallow_char_ = false;
    if (overlay_->HandleKey(*event)) {
      swallow_char_ = true;
      event->StopPropagation();
    }
  }

  void OnWindowDestroying(aura::Window* window) override {
    window_->RemovePreTargetHandler(this);
    observation_.Reset();
    window_ = nullptr;
  }

 private:
  raw_ptr<aura::Window> window_;
  const raw_ptr<CaptureOverlayView> overlay_;
  bool swallow_char_ = false;
  base::ScopedObservation<aura::Window, aura::WindowObserver> observation_{
      this};
};

PageCaptureUI::PageCaptureUI(
    content::WebContents& contents,
    PageCaptureOptions options,
    base::RepeatingCallback<void(gfx::Point)> hover,
    base::RepeatingCallback<void(gfx::Rect, PageCaptureAction)> region,
    base::RepeatingClosure full_page,
    base::RepeatingClosure cancel) {
  auto overlay = std::make_unique<CaptureOverlayView>(
      options, std::move(hover), std::move(region), std::move(full_page),
      std::move(cancel));
  overlay_ = overlay.get();
  widget_ = std::make_unique<views::Widget>();
  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_CONTROL);
  params.parent = contents.GetNativeView();
  params.child = true;
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  params.name = "KryptonPageCapture";
  params.bounds = gfx::Rect(contents.GetViewBounds().size());
  widget_->Init(std::move(params));
  widget_->SetContentsView(std::move(overlay));
  widget_->ShowInactive();
  widget_->StackAtTop();
  keys_ =
      std::make_unique<CaptureKeyHandler>(contents.GetNativeView(), overlay_);
}

PageCaptureUI::~PageCaptureUI() {
  keys_.reset();
  overlay_ = nullptr;
  widget_.reset();
}

void PageCaptureUI::SetHover(std::optional<gfx::Rect> bounds) {
  overlay_->SetHover(bounds);
}

void PageCaptureUI::SetFullPageUnavailable(const std::u16string& reason) {
  overlay_->SetFullPageUnavailable(reason);
}

void PageCaptureUI::Hide() {
  overlay_->HideOverlay();
  widget_->Hide();
}
}  // namespace krypton_ui
