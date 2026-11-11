// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/multi_page_controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/memory/ptr_util.h"
#include "base/numerics/safe_conversions.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_flags.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_context.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_manager.h"
#include "chrome/browser/ui/exclusive_access/fullscreen_controller.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/multi_page_menu.h"
#include "chrome/browser/ui/krypton_ui/multi_page_protocol.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "chrome/browser/ui/krypton_ui/native_backdrop.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/tabs/public/split_tab_data.h"
#include "components/tabs/public/tab_interface.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_animation_observer.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animator.h"
#include "ui/compositor/layer_tree_owner.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace krypton_ui {
bool BrowserAdapter::MultiPageEnabled() const {
  return multi_page_capable_;
}
bool BrowserAdapter::SnapLayoutsEnabled() const {
  const auto* api = session_.api();
  return MultiPageEnabled() && api && HasSnapLayoutProtocol(*api);
}
bool BrowserAdapter::PrepareMultiPageSelection(content::WebContents* contents) {
  if (!multi_page_)
    return false;
  if (auto* selected = contents
                           ? tabs::TabInterface::MaybeGetFromContents(contents)
                           : nullptr;
      selected && !tabs_.contains(Identity(*selected))) {
    // BrowserView can observe activation before our insertion observer. Publish
    // missing identities in model order before any group/Glance update.
    auto* model = browser_->GetTabStripModel();
    for (int index = 0; index < model->count(); ++index) {
      auto* tab = model->GetTabAtIndex(index);
      if (!tabs_.contains(Identity(*tab)))
        Insert(*tab, base::checked_cast<size_t>(index));
    }
  }
  return multi_page_->PrepareSelection(contents);
}
content::WebContents* BrowserAdapter::OpenGlance(
    NavigateParams& params,
    base::OnceCallback<void(content::NavigationHandle&)> callback) {
  if (!MultiPageEnabled())
    return nullptr;
  if (!multi_page_)
    multi_page_ = std::make_unique<MultiPageController>(*this);
  return multi_page_->OpenGlance(params, std::move(callback));
}
bool BrowserAdapter::JoinNativeSplit(KrTabId pivot,
                                     KrTabId other,
                                     KrSplitLayout layout) {
  if (!MultiPageEnabled())
    return false;
  // Callers can be inside a tab strip mutation, and follow the request with
  // their own activation. Join once the current task has finished.
  return session_.Defer(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> self, KrTabId pivot, KrTabId other,
         KrSplitLayout layout) {
        if (!self || !self->MultiPageEnabled())
          return;
        if (!self->multi_page_)
          self->multi_page_ = std::make_unique<MultiPageController>(*self);
        (void)self->multi_page_->JoinSplit(pivot, other, layout);
      },
      weak_factory_.GetWeakPtr(), pivot, other, layout));
}
bool BrowserAdapter::ShowSplitChooser() {
  const auto* api = session_.api();
  return MultiPageEnabled() && api &&
         api->show_split_chooser(api->self) == KR_OK;
}

bool BrowserAdapter::ShowSplitOptions(KrTabId tab) {
  const auto* api = session_.api();
  if (!MultiPageEnabled() || !api ||
      !KR_HAS_SLOT(api, KrShellUI, show_split_options) ||
      !api->show_split_options)
    return false;
  auto point = display::Screen::Get()->GetCursorScreenPoint();
  views::View::ConvertPointFromScreen(&*view_, &point);
  const KrRect anchor{static_cast<float>(point.x()),
                      static_cast<float>(point.y()), 1, 1};
  return session_.Defer(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> self, KrTabId tab, KrRect anchor) {
        if (!self || !self->FindTab(tab))
          return;
        if (const auto* api = self->session_.api();
            api && KR_HAS_SLOT(api, KrShellUI, show_split_options) &&
            api->show_split_options)
          std::ignore = api->show_split_options(api->self, tab, anchor);
      },
      weak_factory_.GetWeakPtr(), tab, anchor));
}
KrResult BrowserAdapter::MultiPageCommand(const KrMultiPageCommand& command) {
  if (!MultiPageEnabled())
    return KR_ERR_UNSUPPORTED;
  if (!KR_HAS_FIELD(&command, KrMultiPageCommand, url) ||
      (!command.url.data && command.url.size) || command.member_count > 4 ||
      (command.member_count &&
       (!command.members || command.member_size < offsetof(KrSplitMember, tab) +
                                                      sizeof(KrTabId))) ||
      (command.edit && !KR_HAS_FIELD(command.edit, KrSplitEdit, weight)))
    return KR_ERR_INVALID_ARGUMENT;
  struct Queued {
    KrMultiPageCommand command;
    std::array<KrSplitMember, 4> members{};
    std::optional<KrSplitEdit> edit;
    std::string url;
  } queued{.command = command};
  if (command.member_count) {
    if (command.member_size >
        static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()) /
            command.member_count)
      return KR_ERR_INVALID_ARGUMENT;
    const auto* bytes = reinterpret_cast<const std::byte*>(command.members);
    for (size_t i = 0; i < command.member_count; ++i) {
      size_t size = 0;
      std::memcpy(&size, bytes + i * command.member_size, sizeof(size));
      if (size < offsetof(KrSplitMember, tab) + sizeof(KrTabId) ||
          size > command.member_size)
        return KR_ERR_INVALID_ARGUMENT;
      std::memcpy(&queued.members[i], bytes + i * command.member_size,
                  std::min(size, sizeof(KrSplitMember)));
      queued.members[i].struct_size = sizeof(KrSplitMember);
    }
  }
  if (command.edit) {
    queued.edit.emplace();
    std::memcpy(&*queued.edit, command.edit,
                std::min(command.edit->struct_size, sizeof(KrSplitEdit)));
    queued.edit->struct_size = sizeof(KrSplitEdit);
  }
  queued.url.assign(command.url.data ? command.url.data : "", command.url.size);
  return session_.Defer(base::BindOnce(
             [](base::WeakPtr<BrowserAdapter> self, Queued queued) {
               if (!self || !self->MultiPageEnabled())
                 return;
               queued.command.members = queued.members.data();
               queued.command.member_size = sizeof(KrSplitMember);
               queued.command.edit = queued.edit ? &*queued.edit : nullptr;
               queued.command.url = {queued.url.data(), queued.url.size()};
               if (!self->multi_page_)
                 self->multi_page_ =
                     std::make_unique<MultiPageController>(*self);
               (void)self->multi_page_->Command(queued.command);
             },
             weak_factory_.GetWeakPtr(), std::move(queued)))
             ? KR_OK
             : KR_ERR_PLATFORM;
}
namespace {
gfx::Rect Bounds(KrRect rect) {
  // Enclose shared boundaries consistently: neighboring pane edges meet the
  // divider at exactly the same DIP, including fractional scale factors.
  return gfx::ToEnclosedRect(
      gfx::RectF(rect.x, rect.y, rect.width, rect.height));
}
MultiPageController* ControllerFor(content::WebContents* contents) {
  if (!base::FeatureList::IsEnabled(kKryptonMultiPage))
    return nullptr;
  auto* tab =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  auto* browser = tab ? tab->GetBrowserWindowInterface() : nullptr;
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  return view && view->krypton_ui() ? view->krypton_ui()->multi_page()
                                    : nullptr;
}
}  // namespace

namespace {
bool RichGlanceMotion(views::View& view) {
  auto* compositor = view.layer() ? view.layer()->GetCompositor() : nullptr;
  return compositor && compositor->IsVisible() &&
         compositor->animations_are_enabled() &&
         gfx::Animation::ShouldRenderRichAnimation() &&
         view.GetNativeTheme()->forced_colors() ==
             ui::ColorProviderKey::ForcedColors::kNone;
}
gfx::Transform CenteredScale(gfx::Size size, float scale) {
  gfx::Transform result;
  result.Translate(size.width() / 2.0f, size.height() / 2.0f);
  result.Scale(scale, scale);
  result.Translate(-size.width() / 2.0f, -size.height() / 2.0f);
  return result;
}
}  // namespace

class GlanceButton final : public views::ImageButton {
  METADATA_HEADER(GlanceButton, views::ImageButton)
 public:
  GlanceButton(PressedCallback callback,
               const gfx::VectorIcon& icon,
               const std::u16string& name,
               KrMultiPageMetrics metrics)
      : ImageButton(std::move(callback)), icon_(icon), metrics_(metrics) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetPreferredSize(gfx::Size(base::ClampRound(metrics_.glance_button_size),
                               base::ClampRound(metrics_.glance_button_size)));
    SetImageHorizontalAlignment(ALIGN_CENTER);
    SetImageVerticalAlignment(ALIGN_MIDDLE);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetTooltipText(name);
    GetViewAccessibility().SetName(name);
    views::FocusRing::Install(this);
  }
  void UpdateTheme(const NativeBackdrop& theme) {
    if (fill_ == theme.glance_button() &&
        hover_ == theme.glance_button_hover() &&
        ink_ == theme.glance_button_icon())
      return;
    fill_ = theme.glance_button();
    hover_ = theme.glance_button_hover();
    ink_ = theme.glance_button_icon();
    for (int state = STATE_NORMAL; state < STATE_COUNT; ++state) {
      const auto color =
          state == STATE_DISABLED
              ? SkColorSetA(ink_,
                            base::ClampRound(SkColorGetA(ink_) *
                                             metrics_.glance_disabled_opacity))
              : ink_;
      SetImageModel(
          static_cast<ButtonState>(state),
          ui::ImageModel::FromVectorIcon(
              *icon_, color, base::ClampRound(metrics_.glance_icon_size)));
    }
    SchedulePaint();
  }

 protected:
  void OnPaintBackground(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetState() == STATE_HOVERED || GetState() == STATE_PRESSED
                       ? hover_
                       : fill_);
    const auto size = GetLocalBounds().size();
    canvas->DrawCircle(gfx::PointF(size.width() / 2.0f, size.height() / 2.0f),
                       std::min(size.width(), size.height()) / 2.0f, flags);
  }
  void StateChanged(ButtonState old_state) override {
    ImageButton::StateChanged(old_state);
    const float scale =
        GetState() == STATE_PRESSED   ? metrics_.glance_press_scale
        : GetState() == STATE_HOVERED ? metrics_.glance_hover_scale
                                      : 1;
    ui::ScopedLayerAnimationSettings settings(layer()->GetAnimator());
    settings.SetPreemptionStrategy(
        ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
    settings.SetTransitionDuration(
        RichGlanceMotion(*this)
            ? base::Milliseconds(metrics_.glance_button_motion_ms)
            : base::TimeDelta());
    SetTransform(CenteredScale(size(), scale));
    SchedulePaint();
  }

 private:
  const raw_ref<const gfx::VectorIcon> icon_;
  const KrMultiPageMetrics metrics_;
  SkColor fill_ = 0, hover_ = 0, ink_ = 0;
};
BEGIN_METADATA(GlanceButton)
END_METADATA

class GlanceControlsView final : public views::View {
  METADATA_HEADER(GlanceControlsView, views::View)
 public:
  explicit GlanceControlsView(KrMultiPageMetrics metrics) : metrics_(metrics) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical,
        gfx::Insets(base::ClampRound(metrics.glance_controls_padding)),
        base::ClampRound(metrics.glance_button_gap)));
    SetVisible(false);
  }
  void UpdateTheme(const NativeBackdrop& theme) {
    for (const auto& child : children())
      static_cast<GlanceButton*>(child.get())->UpdateTheme(theme);
    if (shadow_color_ != theme.glance_button_shadow()) {
      shadow_color_ = theme.glance_button_shadow();
      shadow_looper_ = gfx::CreateShadowDrawLooper({gfx::ShadowValue(
          gfx::Vector2d(), metrics_.glance_button_shadow_blur, shadow_color_)});
      SchedulePaint();
    }
  }
  void SetSide(bool left) { metrics_.glance_controls_on_left = left; }
  void AnimateEntrance() {
    if (!RichGlanceMotion(*this))
      return;
    auto* animator = layer()->GetAnimator();
    animator->StopAnimating();
    layer()->SetOpacity(0);
    gfx::Transform start;
    start.Translate(metrics_.glance_controls_on_left
                        ? metrics_.glance_controls_offset
                        : -metrics_.glance_controls_offset,
                    0);
    SetTransform(start);
    ui::ScopedLayerAnimationSettings settings(animator);
    settings.SetPreemptionStrategy(ui::LayerAnimator::ENQUEUE_NEW_ANIMATION);
    settings.SetTransitionDuration(
        base::Milliseconds(metrics_.glance_controls_ms));
    settings.SetTweenType(gfx::Tween::EASE_OUT);
    animator->SchedulePauseForProperties(
        base::Milliseconds(metrics_.glance_motion_ms -
                           metrics_.glance_controls_ms),
        ui::LayerAnimationElement::OPACITY |
            ui::LayerAnimationElement::TRANSFORM);
    layer()->SetOpacity(1);
    SetTransform(gfx::Transform());
  }

 protected:
  void OnPaintBackground(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(SK_ColorTRANSPARENT);
    flags.setLooper(shadow_looper_);
    // Paint glows in the padded column so button layers do not clip them.
    for (const auto& child : children()) {
      const auto bounds = child->bounds();
      canvas->DrawCircle(gfx::PointF(bounds.CenterPoint()),
                         std::min(bounds.width(), bounds.height()) / 2.0f,
                         flags);
    }
  }
  void VisibilityChanged(views::View* starting_from, bool visible) override {
    views::View::VisibilityChanged(starting_from, visible);
    if (!visible)
      return;
    auto* animator = layer()->GetAnimator();
    animator->StopAnimating();
    layer()->SetOpacity(1);
    SetTransform(gfx::Transform());
  }

 private:
  KrMultiPageMetrics metrics_;
  SkColor shadow_color_ = 0;
  sk_sp<cc::DrawLooper> shadow_looper_;
};
BEGIN_METADATA(GlanceControlsView)
END_METADATA

// Above the parent's native page surface, below the preview and its controls.
class GlanceBackdrop final : public views::View {
  METADATA_HEADER(GlanceBackdrop, views::View)
 public:
  explicit GlanceBackdrop(base::RepeatingClosure close)
      : close_(std::move(close)) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    GetViewAccessibility().SetIsIgnored(true);
    SetVisible(false);
  }
  void Update(gfx::Rect card,
              float radius,
              SkColor shadow,
              const KrMultiPageMetrics& metrics) {
    if (shadow_ != shadow || blur_ != metrics.glance_shadow_blur ||
        offset_ != metrics.glance_shadow_offset) {
      shadow_ = shadow;
      blur_ = metrics.glance_shadow_blur;
      offset_ = metrics.glance_shadow_offset;
      shadow_looper_ = gfx::CreateShadowDrawLooper({gfx::ShadowValue(
          gfx::Vector2d(0, base::ClampRound(offset_)), blur_, shadow_)});
    }
    if (card_ == card && radius_ == radius && !dirty_)
      return;
    card_ = card;
    radius_ = radius;
    dirty_ = false;
    SchedulePaint();
  }
  void ThemeChanged() {
    dirty_ = true;
    SchedulePaint();
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton())
      return true;
    if (!card_.Contains(event.location()))
      close_.Run();
    return true;
  }
  void OnGestureEvent(ui::GestureEvent* event) override {
    if (event->type() == ui::EventType::kGestureTap &&
        !card_.Contains(event->location()))
      close_.Run();
    event->SetHandled();
  }
  void OnPaint(gfx::Canvas* canvas) override {
    if (!SkColorGetA(shadow_))
      return;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    // Paint only the shadow. The live card can move away during its genie
    // transition without exposing an opaque shadow caster behind it.
    canvas->Save();
    canvas->ClipPath(
        SkPath::RRect(gfx::RectFToSkRect(gfx::RectF(card_)), radius_, radius_),
        true, SkClipOp::kDifference);
    flags.setColor(SK_ColorBLACK);
    flags.setLooper(shadow_looper_);
    canvas->DrawRoundRect(gfx::RectF(card_), radius_, flags);
    canvas->Restore();
  }

 private:
  const base::RepeatingClosure close_;
  gfx::Rect card_;
  float radius_ = 0, blur_ = 0, offset_ = 0;
  SkColor shadow_ = 0;
  bool dirty_ = true;
  sk_sp<cc::DrawLooper> shadow_looper_;
};
BEGIN_METADATA(GlanceBackdrop)
END_METADATA

namespace {
// Rows of the lamp. The reference effect warps a 30px grid; projective strips
// follow its curves at a few dozen DIPs, and each strip costs a render pass
// because its clip is not axis-aligned. Strips overlap by a pixel so edge
// antialiasing leaves no seams.
constexpr int kGenieStripLength = 36;
constexpr int kGenieMaxStrips = 24;
constexpr int kGenieSeam = 1;

float InOutSine(float t) {
  return 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * t);
}

float LargestRadius(const ui::Layer& layer) {
  const auto& radii = layer.rounded_corner_radii();
  float radius = std::max({radii.upper_left(), radii.upper_right(),
                           radii.lower_right(), radii.lower_left()});
  for (const ui::Layer* child : layer.children()) {
    if (child->GetTargetVisibility())
      radius = std::max(radius, LargestRadius(*child));
  }
  return radius;
}

// One strip's copy of the card's visible layers. Surface and solid color
// layers are mirrored, so the page stays live and nothing is captured. Painted
// layers become empty placeholders that keep their geometry and clips: a
// mirror would repaint the card's views and raster them again for every
// strip. Rounded corners stay only where `corners` says the strip shows the
// card's corners; elsewhere they would cost another offscreen pass.
std::unique_ptr<ui::Layer> StripTree(ui::Layer& source, bool corners) {
  std::unique_ptr<ui::Layer> layer;
  if (source.type() == ui::LAYER_TEXTURED) {
    layer = ui::Layer::Create(ui::LAYER_NOT_DRAWN);
    layer->SetBounds(source.bounds());
    layer->SetTransform(source.GetTargetTransform());
    layer->SetOpacity(source.GetTargetOpacity());
    layer->SetMasksToBounds(source.GetMasksToBounds());
    layer->SetClipRect(source.GetTargetClipRect());
    layer->SetRoundedCornerRadius(source.GetTargetRoundedCornerRadius());
    layer->SetIsFastRoundedCorner(source.is_fast_rounded_corner());
  } else {
    layer = source.Mirror();
  }
  if (!corners)
    layer->SetRoundedCornerRadius({});
  for (ui::Layer* child : source.children()) {
    if (child->GetTargetVisibility())
      layer->Add(StripTree(*child, corners).release());
  }
  return layer;
}

// The projective transform taking the rect (0, 0, width, height) onto `quad`,
// whose corners follow the rect's: top-left, top-right, bottom-right,
// bottom-left (Heckbert's square-to-quad mapping).
gfx::Transform RectToQuad(float width,
                          float height,
                          const std::array<gfx::PointF, 4>& quad) {
  const double x0 = quad[0].x(), y0 = quad[0].y();
  const double x1 = quad[1].x(), y1 = quad[1].y();
  const double x2 = quad[2].x(), y2 = quad[2].y();
  const double x3 = quad[3].x(), y3 = quad[3].y();
  const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
  const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
  const double det = dx1 * dy2 - dx2 * dy1;
  double g = 0, h = 0;
  if ((std::abs(sx) > 1e-6 || std::abs(sy) > 1e-6) && std::abs(det) > 1e-9) {
    g = (sx * dy2 - dx2 * sy) / det;
    h = (dx1 * sy - sx * dy1) / det;
  }
  const double a = x1 - x0 + g * x1, b = x3 - x0 + h * x3;
  const double d = y1 - y0 + g * y1, e = y3 - y0 + h * y3;
  // clang-format off
  return gfx::Transform::RowMajor(
      a / width, b / height, 0, x0,
      d / width, e / height, 0, y0,
      0,         0,          1, 0,
      g / width, h / height, 0, 1);
  // clang-format on
}
}  // namespace

// Glance's magic lamp: the card pours out of, and back into, a small source
// rect on its bottom edge, under the pointer that opened it. The motion is the
// model of reference/kwin-effects-yet-another-magic-lamp (Model.cc). Closing
// first stretches each row toward its proportional place in the source,
// weighted by an InOutSine of the row's distance from the top, then squashes
// the rows down into the source, where the card's bounds clip them. Opening
// runs the same linear timeline backwards.
//
// Each row is a strip: the card's live layers clipped to the row, with a
// projective transform onto the row's quad. Adjacent strips share edges, the
// page keeps rendering, and no image of it is captured. Per frame, the only
// work is one transform per visible strip.
class GlanceGenie final : public ui::CompositorAnimationObserver,
                          public views::ViewObserver {
 public:
  struct Spec {
    // Card-local position of the pointer that opened the Glance.
    gfx::PointF source;
    // Source size as fractions of the card's width and height.
    float source_width = 0;
    float source_depth = 0;
    // Share of the motion spent stretching before squashing.
    float stretch_share = 0;
    base::TimeDelta duration;
    bool opening = false;
  };

  // Null when the card has no layer, compositor or size to animate.
  static std::unique_ptr<GlanceGenie> Create(views::View& card,
                                             const Spec& spec) {
    ui::Layer* layer = card.layer();
    if (!layer || !layer->GetCompositor() || layer->bounds().IsEmpty())
      return nullptr;
    return base::WrapUnique(new GlanceGenie(card, spec));
  }
  GlanceGenie(const GlanceGenie&) = delete;
  GlanceGenie& operator=(const GlanceGenie&) = delete;
  ~GlanceGenie() override { Detach(spec_.opening); }

  void Start(base::OnceClosure done) {
    done_ = std::move(done);
    Apply(spec_.opening ? 1 : 0);
    compositor_->AddAnimationObserver(this);
    observing_ = true;
  }

  // Ends in the final state at once and runs the completion.
  void Finish() {
    if (finished_)
      return;
    finished_ = true;
    // A closed card stays hidden until its page closes.
    Detach(spec_.opening);
    if (done_)
      std::move(done_).Run();
  }

 private:
  struct Strip {
    std::unique_ptr<ui::Layer> clip;
    std::unique_ptr<ui::LayerTreeOwner> content;
    gfx::Rect band;
  };

  GlanceGenie(views::View& card, const Spec& spec)
      : card_(&card), compositor_(card.layer()->GetCompositor()), spec_(spec) {
    ui::Layer& source = *card.layer();
    const gfx::Rect bounds = source.bounds();
    width_ = bounds.width();
    height_ = bounds.height();
    source_width_ = std::clamp(width_ * spec.source_width, 1.0f,
                               static_cast<float>(width_));
    const float depth =
        std::clamp(height_ * spec.source_depth, 0.0f, height_ / 2.0f);
    distance_ = std::max(1.0f, height_ - depth);
    source_start_ = std::clamp(spec.source.x() - source_width_ / 2, 0.0f,
                               width_ - source_width_);

    root_ = ui::Layer::Create(ui::LAYER_NOT_DRAWN);
    root_->SetName("GlanceGenie");
    root_->SetBounds(bounds);
    // Rows leave through the source, which lies on the card's bottom edge.
    root_->SetMasksToBounds(true);
    const float radius = LargestRadius(source);
    const int count =
        std::clamp((height_ + kGenieStripLength - 1) / kGenieStripLength, 1,
                   kGenieMaxStrips);
    strips_.reserve(count);
    for (int i = 0; i < count; ++i) {
      const int top = height_ * i / count;
      const int bottom =
          std::min(height_, height_ * (i + 1) / count +
                                (i + 1 < count ? kGenieSeam : 0));
      Strip strip;
      strip.band = gfx::Rect(0, top, width_, bottom - top);
      strip.clip = ui::Layer::Create(ui::LAYER_NOT_DRAWN);
      strip.clip->SetName("GlanceGenieStrip");
      strip.clip->SetBounds(strip.band);
      strip.clip->SetMasksToBounds(true);
      auto content =
          StripTree(source, top < radius || bottom > height_ - radius);
      content->SetTransform(gfx::Transform());
      content->SetOpacity(1);
      content->SetBounds(gfx::Rect(0, -top, width_, height_));
      strip.clip->Add(content.get());
      strip.content = std::make_unique<ui::LayerTreeOwner>(std::move(content));
      root_->Add(strip.clip.get());
      strips_.push_back(std::move(strip));
    }
    card.AddLayerToRegion(root_.get(), views::LayerRegion::kAbove);
    source.SetOpacity(0);
    observation_.Observe(&card);
  }

  // ui::CompositorAnimationObserver:
  void OnAnimationStep(base::TimeTicks timestamp) override {
    if (finished_)
      return;
    if (start_.is_null())
      start_ = timestamp;
    const float progress =
        spec_.duration.is_positive()
            ? static_cast<float>(
                  std::clamp((timestamp - start_) / spec_.duration, 0.0, 1.0))
            : 1.0f;
    Apply(spec_.opening ? 1 - progress : progress);
    if (progress < 1)
      return;
    compositor_->RemoveAnimationObserver(this);
    observing_ = false;
    // Tear the strips down after this frame's step, not inside it.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&GlanceGenie::Finish, weak_factory_.GetWeakPtr()));
  }
  void OnCompositingShuttingDown(ui::Compositor* compositor) override {
    observing_ = false;
    compositor_ = nullptr;
    Finish();
  }

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* view) override {
    // A moved card keeps its lamp; a resized one no longer fits it.
    if (view->layer() &&
        view->layer()->bounds().size() == root_->bounds().size()) {
      root_->SetBounds(view->layer()->bounds());
      return;
    }
    Finish();
  }
  void OnViewIsDeleting(views::View* view) override { Finish(); }

  // `closing` runs from 0, the card in place, to 1, the card in the source.
  void Apply(float closing) {
    const float share = std::clamp(spec_.stretch_share, 0.0f, 1.0f);
    float stretch = 1, squash = 0;
    if (closing < share)
      stretch = closing / share;
    else if (share < 1)
      squash = (closing - share) / (1 - share);
    for (auto& strip : strips_) {
      const gfx::RectF band(strip.band);
      const std::array<gfx::PointF, 4> corners = {
          band.origin(), band.top_right(), band.bottom_right(),
          band.bottom_left()};
      std::array<gfx::PointF, 4> quad;
      for (size_t i = 0; i < corners.size(); ++i)
        quad[i] = Warp(corners[i], stretch, squash) - band.OffsetFromOrigin();
      // A strip wholly past the source is clipped away; skip drawing it.
      const bool visible = band.y() + squash * distance_ < height_;
      strip.clip->SetVisible(visible);
      if (visible)
        strip.clip->SetTransform(RectToQuad(band.width(), band.height(), quad));
    }
  }

  // Model.cc's transformQuadsBottom: a point moves `squash` of the way down to
  // the source and toward its place inside it by `stretch` times the shape
  // curve of its new height.
  gfx::PointF Warp(gfx::PointF point, float stretch, float squash) const {
    const float y = point.y() + squash * distance_;
    const float weight =
        stretch * InOutSine(std::clamp(y / distance_, 0.0f, 1.0f));
    const float target = source_start_ + source_width_ * point.x() / width_;
    return {point.x() + weight * (target - point.x()), y};
  }

  void Detach(bool show_card) {
    if (compositor_ && observing_)
      compositor_->RemoveAnimationObserver(this);
    observing_ = false;
    observation_.Reset();
    if (card_) {
      if (root_)
        card_->RemoveLayerFromRegions(root_.get());
      if (show_card && card_->layer())
        card_->layer()->SetOpacity(1);
      card_ = nullptr;
    }
    strips_.clear();
    root_.reset();
  }

  raw_ptr<views::View> card_;
  raw_ptr<ui::Compositor> compositor_;
  const Spec spec_;
  int width_ = 0, height_ = 0;
  float source_start_ = 0, source_width_ = 0, distance_ = 0;
  std::unique_ptr<ui::Layer> root_;
  std::vector<Strip> strips_;
  base::TimeTicks start_;
  base::OnceClosure done_;
  bool observing_ = false;
  bool finished_ = false;
  base::ScopedObservation<views::View, views::ViewObserver> observation_{this};
  base::WeakPtrFactory<GlanceGenie> weak_factory_{this};
};

// Compatibility with modules that predate the appended Glance metrics.
class GlanceToolbarView final : public views::View {
  METADATA_HEADER(GlanceToolbarView, views::View)
 public:
  explicit GlanceToolbarView(uint32_t motion_ms) : motion_ms_(motion_ms) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetBackground(views::CreateSolidBackground(ui::kColorMenuBackground));
  }

 protected:
  void VisibilityChanged(views::View* starting_from, bool visible) override {
    views::View::VisibilityChanged(starting_from, visible);
    if (!visible || !gfx::Animation::ShouldRenderRichAnimation())
      return;
    // Applied after the final page-container bounds. Only this native chrome
    // layer's opacity changes on animation ticks; WebContents is never resized.
    layer()->GetAnimator()->StopAnimating();
    layer()->SetOpacity(0);
    ui::ScopedLayerAnimationSettings settings(layer()->GetAnimator());
    settings.SetTransitionDuration(base::Milliseconds(motion_ms_));
    layer()->SetOpacity(1);
  }

 private:
  const uint32_t motion_ms_;
};
BEGIN_METADATA(GlanceToolbarView)
END_METADATA

// Host-owned chrome: revealing controls never requests native or DOM focus.
class PaneDragHandle final : public views::View {
  METADATA_HEADER(PaneDragHandle, views::View)
 public:
  explicit PaneDragHandle(base::WeakPtr<MultiPageController> owner)
      : owner_(std::move(owner)) {
    SetFocusBehavior(FocusBehavior::NEVER);
    GetViewAccessibility().SetName(u"Drag to move pane");
    SetTooltipText(u"Drag to move pane");
  }
  ~PaneDragHandle() override { Cancel(); }
  void Style(SkColor color, float stroke, float gap) {
    if (color_ == color && stroke_ == stroke && gap_ == gap)
      return;
    color_ = color;
    stroke_ = stroke;
    gap_ = gap;
    SchedulePaint();
  }
  void SetIdentity(KrTabId tab, KrSplitGroupId group, uint64_t revision) {
    if (tab_ != tab || group_ != group || revision_ != revision)
      Cancel();
    tab_ = tab;
    group_ = group;
    revision_ = revision;
  }
  bool pressed() const { return pressed_; }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton() || event.IsControlDown() ||
        event.IsAltDown() || event.IsShiftDown())
      return false;
    Cancel();
    start_ = event.location();
    pressed_ = true;
    dragging_ = false;
    return true;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!pressed_)
      return false;
    if (!event.IsOnlyLeftMouseButton() || event.IsControlDown() ||
        event.IsAltDown() || event.IsShiftDown()) {
      Cancel();
      return true;
    }
    if (!dragging_ && ExceededDragThreshold(event.location() - start_)) {
      dragging_ = owner_ && owner_->BeginPaneDrag(tab_, group_, revision_);
      if (!dragging_) {
        Cancel();
        return true;
      }
    }
    if (dragging_ && owner_) {
      auto screen = event.location();
      ConvertPointToScreen(this, &screen);
      owner_->UpdatePaneDrag(tab_, group_, revision_, screen);
    }
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    const bool drop = std::exchange(pressed_, false) && dragging_;
    dragging_ = false;
    if (!drop || !owner_ || !event.IsLeftMouseButton() ||
        event.IsControlDown() || event.IsAltDown() || event.IsShiftDown()) {
      Cancel();
      return;
    }
    auto screen = event.location();
    ConvertPointToScreen(this, &screen);
    owner_->DropPane(tab_, group_, revision_, screen);
  }
  void OnMouseCaptureLost() override { Cancel(); }
  ui::Cursor GetCursor(const ui::MouseEvent&) override {
    return ui::Cursor(ui::mojom::CursorType::kMove);
  }

 protected:
  void VisibilityChanged(views::View* starting_from, bool visible) override {
    views::View::VisibilityChanged(starting_from, visible);
    if (!IsDrawn())
      Cancel();
  }
  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(color_);
    const auto center = gfx::RectF(GetLocalBounds()).CenterPoint();
    for (int row = -1; row <= 1; ++row)
      for (int column = 0; column < 2; ++column)
        canvas->DrawCircle(
            {center.x() + (column - 0.5f) * gap_, center.y() + row * gap_},
            stroke_ / 2, flags);
  }

 private:
  void Cancel() {
    pressed_ = dragging_ = false;
    if (owner_)
      owner_->CancelPaneDrag(tab_, group_, revision_);
  }
  base::WeakPtr<MultiPageController> owner_;
  KrTabId tab_ = 0;
  KrSplitGroupId group_ = 0;
  uint64_t revision_ = 0;
  gfx::Point start_;
  bool pressed_ = false, dragging_ = false;
  SkColor color_ = SK_ColorTRANSPARENT;
  float stroke_ = 0, gap_ = 0;
};

BEGIN_METADATA(PaneDragHandle)
END_METADATA

class PaneHeader final : public views::View {
  METADATA_HEADER(PaneHeader, views::View)
 public:
  PaneHeader(base::WeakPtr<MultiPageController> owner,
             base::RepeatingClosure menu)
      : owner_(std::move(owner)) {
    SetFocusBehavior(FocusBehavior::NEVER);
    SetNotifyEnterExitOnChild(true);
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal));
    drag_ = AddChildView(std::make_unique<PaneDragHandle>(owner_));
    layout->SetFlexForView(drag_, 1);
    expand_ = AddChildView(std::make_unique<views::LabelButton>(
        base::BindRepeating(&PaneHeader::Expand, base::Unretained(this)),
        u"\u2922"));
    expand_->GetViewAccessibility().SetName(u"Expand pane");
    menu_ = AddChildView(
        std::make_unique<views::LabelButton>(std::move(menu), u"\u22ef"));
    menu_->GetViewAccessibility().SetName(u"Pane options");
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    Reveal(false);
  }
  void Update(KrTabId tab,
              KrSplitGroupId group,
              uint64_t revision,
              const NativeBackdrop& theme,
              const KrMultiPageMetrics& metrics,
              float radius,
              int height) {
    tab_ = tab;
    group_ = group;
    revision_ = revision;
    drag_->SetIdentity(tab, group, revision);
    drag_->Style(theme.glance_button_icon(), metrics.outline_width,
                 metrics.divider_width);
    if (background_color_ != theme.glance_button()) {
      background_color_ = theme.glance_button();
      SetBackground(
          views::CreateRoundedRectBackground(background_color_, radius));
    }
    expand_->SetEnabledTextColors(theme.glance_button_icon());
    menu_->SetEnabledTextColors(theme.glance_button_icon());
    const auto side = height;
    expand_->SetPreferredSize(gfx::Size(side, side));
    menu_->SetPreferredSize(gfx::Size(side, side));
  }
  void OnMouseEntered(const ui::MouseEvent&) override { Reveal(true); }
  void OnMouseExited(const ui::MouseEvent&) override {
    const auto* focus = GetFocusManager();
    if (!drag_->pressed() && !(focus && Contains(focus->GetFocusedView())))
      Reveal(false);
  }

 private:
  void Reveal(bool reveal) {
    // The hit region remains stable; hidden buttons cannot receive keys.
    expand_->SetVisible(reveal);
    menu_->SetVisible(reveal);
    drag_->SetVisible(reveal);
    layer()->SetOpacity(reveal ? 1.f : 0.f);
  }
  void Expand() {
    if (owner_)
      owner_->ExpandPane(tab_, group_, revision_);
  }
  base::WeakPtr<MultiPageController> owner_;
  raw_ptr<PaneDragHandle> drag_ = nullptr;
  raw_ptr<views::LabelButton> expand_ = nullptr, menu_ = nullptr;
  KrTabId tab_ = 0;
  KrSplitGroupId group_ = 0;
  uint64_t revision_ = 0;
  SkColor background_color_ = SK_ColorTRANSPARENT;
};

BEGIN_METADATA(PaneHeader)
END_METADATA

class PaneDivider final : public views::View {
  METADATA_HEADER(PaneDivider, views::View)
 public:
  PaneDivider(base::WeakPtr<MultiPageController> owner, size_t index)
      : owner_(std::move(owner)), index_(index) {
    SetFocusBehavior(FocusBehavior::ALWAYS);
    views::FocusRing::Install(this);
    GetViewAccessibility().SetRole(ax::mojom::Role::kSplitter);
    GetViewAccessibility().SetName(u"Resize split panes");
  }
  void Update(KrDividerBounds bounds,
              bool rtl,
              KrSplitGroupId group,
              uint64_t revision) {
    if (dragging_ && (group != group_ || revision != revision_))
      dragging_ = false;
    bounds_ = bounds;
    rtl_ = rtl;
    if (!dragging_) {
      group_ = group;
      revision_ = revision;
    }
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton())
      return false;
    RequestFocus();
    dragging_ = true;
    resized_ = false;
    return true;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    return Resize(event, false);
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    (void)Resize(event, true);
  }
  void OnMouseCaptureLost() override {
    if (!std::exchange(dragging_, false) || !resized_ || !owner_)
      return;
    // Losing capture completes the gesture too (for example on Alt+Tab).
    // Persist its last accepted weight even if layout is still coalesced.
    (void)owner_->ResizeDivider(index_, pending_weight_, true, group_,
                                revision_);
  }
  ui::Cursor GetCursor(const ui::MouseEvent&) override {
    return ui::Cursor(bounds_.horizontal
                          ? ui::mojom::CursorType::kRowResize
                          : ui::mojom::CursorType::kColumnResize);
  }
  bool OnKeyPressed(const ui::KeyEvent& event) override {
    const bool decrease =
        event.key_code() == (bounds_.horizontal ? ui::VKEY_UP : ui::VKEY_LEFT);
    const bool increase = event.key_code() ==
                          (bounds_.horizontal ? ui::VKEY_DOWN : ui::VKEY_RIGHT);
    if ((!decrease && !increase) || !owner_)
      return false;
    const auto& available = bounds_.available;
    const float extent = bounds_.horizontal
                             ? available.height - bounds_.bounds.height
                             : available.width - bounds_.bounds.width;
    if (extent <= 0)
      return true;
    float weight = bounds_.horizontal
                       ? (bounds_.bounds.y - available.y) / extent
                       : (bounds_.bounds.x - available.x) / extent;
    weight += increase ? 0.02f : -0.02f;
    if (rtl_ && !bounds_.horizontal)
      weight = 1 - weight;
    const auto revision =
        owner_->ResizeDivider(index_, weight, true, group_, revision_);
    if (revision)
      revision_ = *revision;
    return revision.has_value();
  }

 private:
  bool Resize(const ui::MouseEvent& event, bool complete) {
    if (!owner_ || !parent())
      return false;
    auto point = event.location();
    views::View::ConvertPointToTarget(this, parent(), &point);
    const auto& available = bounds_.available;
    const float extent = bounds_.horizontal
                             ? available.height - bounds_.bounds.height
                             : available.width - bounds_.bounds.width;
    if (extent <= 0)
      return false;
    float weight = (bounds_.horizontal ? point.y() - available.y
                                       : point.x() - available.x) /
                   extent;
    if (rtl_ && !bounds_.horizontal)
      weight = 1 - weight;
    if (!dragging_)
      return false;
    const auto revision =
        owner_->ResizeDivider(index_, weight, complete, group_, revision_);
    if (revision)
      revision_ = *revision;
    dragging_ = revision.has_value() && !complete;
    if (revision) {
      pending_weight_ = weight;
      resized_ = true;
    }
    return revision.has_value();
  }
  base::WeakPtr<MultiPageController> owner_;
  size_t index_;
  KrDividerBounds bounds_{};
  bool rtl_ = false;
  bool dragging_ = false;
  bool resized_ = false;
  float pending_weight_ = 0.5f;
  KrSplitGroupId group_ = 0;
  uint64_t revision_ = 0;
};
BEGIN_METADATA(PaneDivider)
END_METADATA

// The focused page's outline, drawn in the gap just outside its pane so it
// covers no page content. Colour and width are resolved by the module.
class PaneOutline final : public views::View {
  METADATA_HEADER(PaneOutline, views::View)
 public:
  explicit PaneOutline(bool drop_target = false) {
    SetFocusBehavior(FocusBehavior::NEVER);
    SetCanProcessEventsWithinSubtree(false);
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    if (drop_target) {
      auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical));
      layout->set_main_axis_alignment(
          views::BoxLayout::MainAxisAlignment::kCenter);
      layout->set_cross_axis_alignment(
          views::BoxLayout::CrossAxisAlignment::kCenter);
      caption_ = AddChildView(
          std::make_unique<views::Label>(u"Drop to move pane here"));
      caption_->SetFocusBehavior(FocusBehavior::NEVER);
    }
  }
  void DropStyle(const NativeBackdrop& theme,
                 const KrMultiPageMetrics& metrics,
                 float radius) {
    const bool changed = width_ != metrics.outline_width || radius_ != radius ||
                         caption_background_ != theme.glance_button() ||
                         caption_foreground_ != theme.glance_button_icon() ||
                         caption_gap_ != metrics.divider_width;
    Update(theme.pane_outline(), metrics.outline_width, radius);
    if (caption_ && changed) {
      caption_background_ = theme.glance_button();
      caption_foreground_ = theme.glance_button_icon();
      caption_gap_ = metrics.divider_width;
      caption_->SetEnabledColor(theme.glance_button_icon());
      caption_->SetBackground(
          views::CreateRoundedRectBackground(theme.glance_button(), radius));
      caption_->SetBorder(views::CreateEmptyBorder(
          gfx::Insets::VH(base::ClampCeil(metrics.outline_width),
                          base::ClampCeil(metrics.divider_width))));
    }
  }
  void Update(SkColor color, float width, float radius) {
    if (color == color_ && width == width_ && radius == radius_)
      return;
    color_ = color;
    width_ = width;
    radius_ = radius;
    SchedulePaint();
  }
  void OnPaint(gfx::Canvas* canvas) override {
    if (!SkColorGetA(color_) || width_ <= 0)
      return;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(width_);
    flags.setColor(color_);
    gfx::RectF bounds(GetLocalBounds());
    bounds.Inset(width_ / 2);
    canvas->DrawRoundRect(bounds, radius_ + width_ / 2, flags);
  }

 private:
  SkColor color_ = SK_ColorTRANSPARENT;
  float width_ = 0;
  float radius_ = 0;
  raw_ptr<views::Label> caption_ = nullptr;
  SkColor caption_background_ = SK_ColorTRANSPARENT;
  SkColor caption_foreground_ = SK_ColorTRANSPARENT;
  float caption_gap_ = 0;
};
BEGIN_METADATA(PaneOutline)
END_METADATA

MultiPageController::Group::Group() {
  for (auto& member : members)
    member.struct_size = sizeof(member);
  for (auto& weight : weights)
    weight = {sizeof(weight), 0.5f};
}
KrSplitGroup MultiPageController::Group::Record() {
  return {sizeof(KrSplitGroup),
          id,
          space,
          revision,
          layout,
          selected,
          members.data(),
          count,
          sizeof(KrSplitMember),
          weights.data(),
          weights.size(),
          sizeof(KrSplitWeight)};
}
void MultiPageController::Group::Adopt(const KrSplitGroup& record) {
  id = record.id;
  space = record.space;
  revision = record.revision;
  layout = record.layout;
  count = record.member_count;
  selected = record.active_tab;
}
bool MultiPageController::HasCompleteModule(const KrShellUI* api) {
  return api && HasCompleteMultiPageProtocol(*api);
}
bool MultiPageController::HasSnapLayouts() const {
  const auto* api = owner_->session_.api();
  return api && HasSnapLayoutProtocol(*api);
}
MultiPageController::MultiPageController(BrowserAdapter& owner)
    : owner_(owner), view_(owner.view_->multi_contents_view()) {
  CHECK(HasCompleteModule(owner.session_.api()));
  const auto* api = owner.session_.api();
  CHECK_EQ(api->get_multi_page_metrics(api->self, &metrics_), KR_OK);
  for (size_t i = 0; i < 2; ++i) {
    containers_[i] = view_->GetKryptonNativeContainer(i);
    if (auto* contents = containers_[i]->contents_view()->web_contents()) {
      if (auto* tab = tabs::TabInterface::MaybeGetFromContents(contents))
        bound_tabs_[i] = BrowserAdapter::Identity(*tab);
    }
  }
  for (auto& pane : pane_bounds_)
    pane.struct_size = sizeof(pane);
  for (auto& divider : divider_bounds_)
    divider.struct_size = sizeof(divider);
}
MultiPageController::~MultiPageController() {
  ClearPaneDrag();
  weak_factory_.InvalidateWeakPtrs();
  glance_genie_.reset();
  UpdateGlanceParent(nullptr, {}, false);
  for (size_t i = 2; i < containers_.size(); ++i) {
    if (containers_[i]) {
      containers_[i]->contents_view()->SetWebContents(nullptr);
      containers_[i]->SetVisible(false);
    }
  }
}
tabs::TabInterface* MultiPageController::Tab(KrTabId id) const {
  return owner_->FindTab(id);
}
MultiPageController::Group* MultiPageController::GroupFor(KrTabId tab) {
  const auto member = membership_.find(tab);
  if (member == membership_.end())
    return nullptr;
  const auto found = groups_.find(member->second);
  return found == groups_.end() ? nullptr : &found->second;
}
std::optional<MultiPageController::GroupInfo> MultiPageController::Describe(
    KrTabId tab) const {
  const auto member = membership_.find(tab);
  if (member == membership_.end())
    return std::nullopt;
  const auto found = groups_.find(member->second);
  if (found == groups_.end())
    return std::nullopt;
  const auto& group = found->second;
  for (size_t i = 0; i < group.count; ++i)
    if (group.members[i].tab == tab)
      return GroupInfo{group.id,     group.space, group.revision,
                       group.layout, group.count, i};
  return std::nullopt;
}
ContentsContainerView* MultiPageController::EnsureContainer(size_t index) {
  CHECK_LT(index, containers_.size());
  if (!containers_[index])
    containers_[index] = view_->AddKryptonContentsContainer();
  return containers_[index];
}
ContentsContainerView* MultiPageController::ActiveContainer() const {
  return active_ ? containers_[selected_container_].get() : nullptr;
}
ContentsContainerView* MultiPageController::InactiveContainer() const {
  for (size_t i = 0; i < containers_.size(); ++i) {
    if (i != selected_container_ && containers_[i] &&
        containers_[i]->GetVisible())
      return containers_[i];
  }
  return containers_[selected_container_ == 0 ? 1 : 0];
}

KrResult MultiPageController::CreateGroup(std::span<const KrTabId> selected,
                                          KrSplitLayout layout,
                                          KrTabId active) {
  if (selected.size() < 2 || selected.size() > 4)
    return KR_ERR_INVALID_ARGUMENT;
  if (layout < KR_SPLIT_COLUMNS || layout > KR_SPLIT_MAIN_BOTTOM ||
      (layout > KR_SPLIT_GRID && !HasSnapLayouts()))
    return KR_ERR_INVALID_ARGUMENT;
  const auto* api = owner_->session_.api();
  auto* model = owner_->browser_->GetTabStripModel();
  std::array<KrTab, 4> sources{};
  std::array<KrSplitCopyMember, 4> copies{};
  for (size_t i = 0; i < selected.size(); ++i) {
    auto* tab = Tab(selected[i]);
    if (!tab || IsTransientGlance(tab->GetContents()))
      return KR_ERR_INVALID_ARGUMENT;
    const auto flags =
        (tab->IsPinned() ? KR_TAB_PINNED : 0) |
        (EssentialForTab(tab->GetContents()) ? KR_TAB_FAVORITE : 0);
    sources[i] = {.struct_size = sizeof(KrTab),
                  .id = selected[i],
                  .space = WorkspaceForTab(tab->GetContents()),
                  .flags = static_cast<uint32_t>(flags)};
    copies[i].struct_size = sizeof(copies[i]);
  }
  const KrSplitCopyRequest request{sizeof(KrSplitCopyRequest), sources.data(),
                                   selected.size(), sizeof(KrTab),
                                   owner_->active_space_};
  KrSplitCopyPlan copy_plan{sizeof(KrSplitCopyPlan), copies.data(),
                            copies.size(), sizeof(KrSplitCopyMember), false};
  if (api->plan_split_copies(api->self, &request, &copy_plan) != KR_OK)
    return KR_ERR_INVALID_ARGUMENT;
  Group group;
  group.count = 4;  // output capacity; MakeSplit supplies actual count
  std::array<KrSplitMember, 4> members{};
  KrTabId active_member = 0;
  for (size_t i = 0; i < selected.size(); ++i) {
    auto* tab = Tab(selected[i]);
    if (copies[i].copy) {
      auto* copied =
          chrome::DuplicateTabAt(&*owner_->browser_, model->GetIndexOfTab(tab));
      if (!copied)
        return KR_ERR_PLATFORM;
      tab = tabs::TabInterface::GetFromContents(copied);
      owner_->AssignEssential(*tab, 0);
      owner_->AssignWorkspace(*tab, owner_->active_space_);
      model->SetTabPinned(model->GetIndexOfTab(tab), false);
      owner_->PublishTab(*tab);
    }
    // The native split collection and Krypton's bounded group must never own
    // the same tab. Unsplit the native collection without recreating either
    // WebContents before adopting a member into the custom layout.
    if (const auto native_split =
            model->GetSplitForTab(model->GetIndexOfTab(tab)))
      model->RemoveSplit(*native_split);
    const auto id = BrowserAdapter::Identity(*tab);
    if (auto* previous = GroupFor(id)) {
      const auto result =
          EditGroup(previous->id,
                    {sizeof(KrSplitEdit), KR_SPLIT_DETACH, previous->revision,
                     id, previous->space, 0, previous->layout, 0.5f});
      if (result != KR_OK)
        return result;
    }
    members[i] = {sizeof(KrSplitMember), id};
    if (selected[i] == active)
      active_member = id;
  }
  if (!active_member)
    active_member = members[0].tab;
  if (!next_group_ || next_group_ == std::numeric_limits<KrSplitGroupId>::max())
    return KR_ERR_PLATFORM;
  const KrSplitCreateRequest create{sizeof(KrSplitCreateRequest),
                                    next_group_++,
                                    owner_->active_space_,
                                    members.data(),
                                    selected.size(),
                                    sizeof(KrSplitMember),
                                    active_member,
                                    layout};
  auto output = group.Record();
  if (api->make_split(api->self, &create, &output) != KR_OK)
    return KR_ERR_INVALID_ARGUMENT;
  group.Adopt(output);
  const auto id = group.id;
  groups_.insert_or_assign(id, group);
  for (size_t i = 0; i < group.count; ++i)
    membership_.insert_or_assign(group.members[i].tab, id);
  auto& stored = groups_.at(id);
  Publish(stored);
  Persist(stored);
  owner_->Activate(active_member);
  (void)PrepareSelection(Tab(active_member)->GetContents());
  ScheduleLayout();
  return KR_OK;
}

KrResult MultiPageController::EditGroup(KrSplitGroupId id,
                                        const KrSplitEdit& edit,
                                        bool persist) {
  const auto found = groups_.find(id);
  if (found == groups_.end())
    return KR_ERR_INVALID_ARGUMENT;
  const auto* api = owner_->session_.api();
  auto input = found->second.Record();
  Group next;
  next.count = 4;
  auto output = next.Record();
  if (api->reduce_split(api->self, &input, &edit, &output) != KR_OK)
    return KR_ERR_INVALID_ARGUMENT;
  next.Adopt(output);
  if (!next.id) {
    Dissolve(id);
  } else {
    const auto previous = found->second;
    for (size_t i = 0; i < previous.count; ++i) {
      const auto tab = previous.members[i].tab;
      if (std::ranges::none_of(std::span(next.members).first(next.count),
                               [tab](const KrSplitMember& member) {
                                 return member.tab == tab;
                               })) {
        membership_.erase(tab);
        if (auto* removed = Tab(tab)) {
          SetSplitSessionForTab(removed->GetContents(), std::nullopt);
          if (auto* service = SessionServiceFactory::GetForProfile(
                  owner_->browser_->GetProfile()))
            service->AddTabExtraData(
                owner_->browser_->GetSessionID(),
                sessions::SessionTabHelper::IdForTab(removed->GetContents()),
                kSplitSessionKey, "");
        }
      }
    }
    for (size_t i = 0; i < next.count; ++i)
      membership_.insert_or_assign(next.members[i].tab, id);
    groups_.at(id) = next;
    Publish(groups_.at(id));
    if (persist)
      Persist(groups_.at(id));
  }
  if (auto* selected = owner_->browser_->GetActiveTabInterface())
    (void)PrepareSelection(selected->GetContents());
  ScheduleLayout();
  return KR_OK;
}
void MultiPageController::Publish(Group& group) {
  const auto* api = owner_->session_.api();
  const auto record = group.Record();
  CHECK_EQ(api->upsert_split_group(api->self, &record), KR_OK);
}
void MultiPageController::Persist(Group& group) {
  auto* service =
      SessionServiceFactory::GetForProfile(owner_->browser_->GetProfile());
  for (size_t i = 0; i < group.count; ++i) {
    auto* tab = Tab(group.members[i].tab);
    if (!tab)
      continue;
    SplitSessionData data{group.id, base::checked_cast<uint32_t>(group.count),
                          base::checked_cast<uint32_t>(i),
                          static_cast<uint32_t>(group.layout)};
    for (size_t j = 0; j < 3; ++j)
      data.weights[j] = group.weights[j].value;
    SetSplitSessionForTab(tab->GetContents(), data);
    const auto session_id =
        sessions::SessionTabHelper::IdForTab(tab->GetContents());
    if (service && session_id.is_valid())
      service->AddTabExtraData(owner_->browser_->GetSessionID(), session_id,
                               kSplitSessionKey, EncodeSplitSessionData(data));
  }
}
void MultiPageController::Dissolve(KrSplitGroupId id) {
  const auto found = groups_.find(id);
  if (found == groups_.end())
    return;
  const auto previous = found->second;
  const auto* api = owner_->session_.api();
  CHECK_EQ(api->remove_split_group(api->self, id, previous.revision), KR_OK);
  for (size_t i = 0; i < previous.count; ++i) {
    const auto member = previous.members[i].tab;
    membership_.erase(member);
    if (auto* tab = Tab(member)) {
      SetSplitSessionForTab(tab->GetContents(), std::nullopt);
      if (auto* service = SessionServiceFactory::GetForProfile(
              owner_->browser_->GetProfile()))
        service->AddTabExtraData(
            owner_->browser_->GetSessionID(),
            sessions::SessionTabHelper::IdForTab(tab->GetContents()),
            kSplitSessionKey, "");
    }
  }
  groups_.erase(id);
}

void MultiPageController::ScheduleLayout() {
  if (layout_scheduled_)
    return;
  layout_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<MultiPageController> self) {
                       if (!self)
                         return;
                       self->layout_scheduled_ = false;
                       self->view_->InvalidateLayout();
                     },
                     weak_factory_.GetWeakPtr()));
}

void MultiPageController::ReleaseHidden() {
  for (size_t i = 0; i < containers_.size(); ++i) {
    if (!containers_[i] || !bound_tabs_[i])
      continue;
    const auto id = bound_tabs_[i];
    bool keep = glance_.phase != KR_GLANCE_CLOSED &&
                (id == glance_.parent || id == glance_.child);
    if (visible_) {
      keep |= std::ranges::any_of(
          std::span(visible_->members).first(visible_->count),
          [id](const KrSplitMember& member) { return member.tab == id; });
    }
    // Promotion reparents the same WebContents into an ordinary pane. Release
    // its transient WebView first, even when it is now a member of visible_.
    if (i == 4 && glance_.phase == KR_GLANCE_CLOSED)
      keep = false;
    // A newly activated Glance child may still occupy a stock WebView.
    // Release it before attaching its dedicated transient container.
    if (glance_.phase != KR_GLANCE_CLOSED && id == glance_.child && i != 4)
      keep = false;
    if (!keep) {
      if (i == 4 && containers_[i]->layer()) {
        containers_[i]->contents_view()->SetFastResize(
            glance_fast_resize_before_);
        containers_[i]->layer()->GetAnimator()->StopAnimating();
        containers_[i]->SetTransform(gfx::Transform());
        containers_[i]->layer()->SetOpacity(1);
      }
      if (dimmed_parent_ == containers_[i])
        UpdateGlanceParent(nullptr, {}, false);
      containers_[i]->contents_view()->SetWebContents(nullptr);
      containers_[i]->SetVisible(false);
      bound_tabs_[i] = 0;
    }
  }
}
void MultiPageController::BindVisible(tabs::TabInterface* selected) {
  const base::AutoReset<bool> binding(&binding_, true);
  view_->HideKryptonNativeSplitChrome();
  // Chromium still owns the ordinary single-page path while this controller
  // is inactive. Activation and pinned/Essential duplication can replace a
  // stock WebView's page without updating our last group binding. Reconcile
  // from the real owners before releasing or attaching any page: a second
  // WebView for the same page would register two Reading Mode overlays.
  for (size_t slot = 0; slot < containers_.size(); ++slot) {
    if (!containers_[slot])
      continue;
    auto* contents = containers_[slot]->contents_view()->web_contents();
    auto* tab =
        contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
    bound_tabs_[slot] = tab ? BrowserAdapter::Identity(*tab) : 0;
  }
  const KrTabId selected_id = BrowserAdapter::Identity(*selected);
  const KrTabId parent =
      glance_.phase != KR_GLANCE_CLOSED ? glance_.parent : selected_id;
  auto* group = GroupFor(parent);
  visible_ = group ? std::optional(*group) : std::nullopt;
  ReleaseHidden();
  pane_count_ = group ? group->count : 1;
  for (size_t pane = 0; pane < pane_count_; ++pane) {
    const auto id = group ? group->members[pane].tab : parent;
    size_t slot = 0;
    for (; slot < 4; ++slot)
      if (bound_tabs_[slot] == id)
        break;
    if (slot == 4) {
      for (slot = 0; slot < 4; ++slot)
        if (!bound_tabs_[slot])
          break;
      CHECK_LT(slot, 4u);
      auto* tab = Tab(id);
      EnsureContainer(slot)->contents_view()->SetWebContents(
          tab ? tab->GetContents() : nullptr);
      bound_tabs_[slot] = id;
    }
    pane_containers_[pane] = slot;
    containers_[slot]->SetVisible(Tab(id) != nullptr);
    if (id == selected_id)
      selected_container_ = slot;
  }
  if (glance_.phase != KR_GLANCE_CLOSED) {
    auto* child = Tab(glance_.child);
    CHECK(child);
    auto* container = EnsureContainer(4);
    if (bound_tabs_[4] != glance_.child)
      glance_fast_resize_before_ = container->contents_view()->GetFastResize();
    if (container->contents_view()->web_contents() != child->GetContents())
      container->contents_view()->SetWebContents(child->GetContents());
    bound_tabs_[4] = glance_.child;
    container->SetVisible(true);
    selected_container_ = 4;
  }
  for (size_t i = 0; i < containers_.size(); ++i) {
    if (containers_[i])
      containers_[i]->contents_view()->set_is_primary_web_contents_for_window(
          i == selected_container_);
  }
  owner_->SchedulePageBackgrounds();
  active_ = true;
  EnsureChrome();
}
void MultiPageController::OpenPaneMenu(size_t slot) {
  if (slot >= pane_controls_.size() || !bound_tabs_[slot])
    return;
  auto* tab = Tab(bound_tabs_[slot]);
  if (!tab)
    return;
  Focus(tab->GetContents());
  if (!tab->IsActivated())
    return;
  pane_menu_leases_[slot].RunAndReset();
  pane_menus_[slot] =
      std::make_unique<MultiPageMenuModel>(*owner_->browser_, *tab);
  pane_menu_leases_[slot] = owner_->RequestPageMenu(
      *pane_menus_[slot],
      pane_controls_[slot]->GetBoundsInScreen().bottom_left());
}
void MultiPageController::EnsureChrome() {
  if (visible_ && !outline_) {
    outline_ = view_->AddChildView(std::make_unique<PaneOutline>());
    OutlineChanged();
  }
  if (visible_) {
    for (size_t i = 0; i < visible_->count - 1; ++i) {
      if (!dividers_[i])
        dividers_[i] = view_->AddChildView(
            std::make_unique<PaneDivider>(weak_factory_.GetWeakPtr(), i));
    }
    for (size_t i = 0; i < pane_count_; ++i) {
      const auto slot = pane_containers_[i];
      if (pane_controls_[slot])
        continue;
      auto header = std::make_unique<PaneHeader>(
          weak_factory_.GetWeakPtr(),
          base::BindRepeating(
              [](base::WeakPtr<MultiPageController> self, size_t slot) {
                if (self)
                  self->OpenPaneMenu(slot);
              },
              weak_factory_.GetWeakPtr(), slot));
      pane_controls_[slot] = view_->AddChildView(std::move(header));
    }
  }
  if (glance_.phase != KR_GLANCE_CLOSED && !glance_toolbar_) {
    const auto close = base::BindRepeating(
        [](base::WeakPtr<MultiPageController> self) {
          if (self)
            self->RequestCloseGlance();
        },
        weak_factory_.GetWeakPtr());
    const auto expand = base::BindRepeating(
        [](base::WeakPtr<MultiPageController> self) {
          if (self)
            self->PromoteGlance(false);
        },
        weak_factory_.GetWeakPtr());
    const auto split = base::BindRepeating(
        [](base::WeakPtr<MultiPageController> self) {
          if (self)
            self->PromoteGlance(true);
        },
        weak_factory_.GetWeakPtr());
    if (metrics_.glance_button_size > 0 &&
        KR_HAS_SLOT(owner_->session_.api(), KrShellUI, layout_glance_card) &&
        owner_->session_.api()->layout_glance_card) {
      glance_backdrop_ =
          view_->AddChildView(std::make_unique<GlanceBackdrop>(close));
      auto controls = std::make_unique<GlanceControlsView>(metrics_);
      controls->AddChildView(std::make_unique<GlanceButton>(
          close, vector_icons::kCloseIcon, u"Close preview", metrics_));
      controls->AddChildView(std::make_unique<GlanceButton>(
          expand, kOpenInFullIcon, u"Open as tab", metrics_));
      glance_split_button_ =
          controls->AddChildView(std::make_unique<GlanceButton>(
              split, kSplitSceneIcon, u"Split with page", metrics_));
      controls->UpdateTheme(*owner_->backdrop_);
      glance_controls_ = view_->AddChildView(std::move(controls));
      glance_toolbar_ = glance_controls_;
    } else {
      auto toolbar = std::make_unique<GlanceToolbarView>(metrics_.motion_ms);
      toolbar->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal));
      toolbar->AddChildView(
          std::make_unique<views::LabelButton>(close, u"Close"));
      toolbar->AddChildView(
          std::make_unique<views::LabelButton>(expand, u"Expand"));
      glance_split_button_ = toolbar->AddChildView(
          std::make_unique<views::LabelButton>(split, u"Split"));
      glance_toolbar_ = view_->AddChildView(std::move(toolbar));
    }
  }
  if (glance_.phase != KR_GLANCE_CLOSED) {
    if (glance_controls_) {
      for (size_t i = 0; i < containers_.size(); ++i) {
        if (!containers_[i] || (bound_tabs_[i] != glance_.parent && i != 4))
          continue;
        if (!containers_[i]->layer()) {
          containers_[i]->SetPaintToLayer();
          containers_[i]->layer()->SetFillsBoundsOpaquely(false);
        }
      }
    }
    if (glance_backdrop_)
      view_->ReorderChildView(
          glance_backdrop_,
          base::checked_cast<int>(view_->children().size() - 1));
    view_->ReorderChildView(
        containers_[4], base::checked_cast<int>(view_->children().size() - 1));
    view_->ReorderChildView(
        glance_toolbar_, base::checked_cast<int>(view_->children().size() - 1));
  }
}
bool MultiPageController::PrepareSelection(content::WebContents* contents) {
  if (binding_ || !contents)
    return active_;
  pending_focus_ = 0;
  pending_focus_contents_.reset();
  auto* selected = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!selected)
    return false;
  // Browser::OnActiveTabChanged runs before BrowserAdapter receives the
  // removal notification. Closing the active preview has already detached
  // its TabInterface at this point. Retire the transient identity before
  // BindVisible can try to attach that missing child again. Removed is
  // idempotent; the later observer notification still releases normal tabs.
  if (glance_.phase != KR_GLANCE_CLOSED && !Tab(glance_.child))
    Removed(glance_.child);
  const auto id = BrowserAdapter::Identity(*selected);
  if (opening_parent_ && IsTransientGlance(contents) &&
      glance_.phase == KR_GLANCE_CLOSED) {
    const auto* api = owner_->session_.api();
    KrGlanceEdit edit{sizeof(KrGlanceEdit),
                      KR_GLANCE_CREATE,
                      opening_parent_,
                      id,
                      glance_.generation + 1,
                      0,
                      1};
    KrGlanceState next{.struct_size = sizeof(KrGlanceState)};
    CHECK_EQ(api->reduce_glance(api->self, &glance_, &edit, &next), KR_OK);
    glance_ = next;
    PublishGlance();
  }
  const auto parent = glance_.phase != KR_GLANCE_CLOSED && id == glance_.child
                          ? glance_.parent
                          : id;
  auto* group = GroupFor(parent);
  if (!group && glance_.phase == KR_GLANCE_CLOSED) {
    if (!active_)
      return false;
    const base::AutoReset<bool> binding(&binding_, true);
    ClearPaneDrag();
    visible_.reset();
    ReleaseHidden();
    // Native single-page layout does not manage our lazily added chrome.
    if (glance_toolbar_)
      glance_toolbar_->SetVisible(false);
    if (glance_backdrop_)
      glance_backdrop_->SetVisible(false);
    if (outline_)
      outline_->SetVisible(false);
    for (const auto& divider : dividers_)
      if (divider)
        divider->SetVisible(false);
    for (const auto& control : pane_controls_)
      if (control)
        control->SetVisible(false);
    active_ = false;
    EnsureContainer(0)->SetVisible(true);
    view_->SetActiveIndex(0);
    view_->SetWebContentsAtIndex(contents, 0);
    // Chromium owns the stock WebViews again. It can rebind them without
    // notifying this controller, so retaining an identity here would let a
    // later removal/replacement of that old tab hide or overwrite a new page.
    bound_tabs_.fill(0);
    selected_container_ = 0;
    for (size_t slot = 0; slot < containers_.size(); ++slot)
      if (containers_[slot])
        containers_[slot]
            ->contents_view()
            ->set_is_primary_web_contents_for_window(slot == 0);
    ScheduleLayout();
    return false;
  }
  if (group && group->selected != parent) {
    const auto* api = owner_->session_.api();
    auto input = group->Record();
    Group next;
    next.count = 4;
    auto output = next.Record();
    const KrSplitEdit edit{sizeof(KrSplitEdit), KR_SPLIT_ACTIVATE,
                           group->revision,     parent,
                           group->space,        0,
                           group->layout,       0.5f};
    CHECK_EQ(api->reduce_split(api->self, &input, &edit, &output), KR_OK);
    next.Adopt(output);
    *group = next;
    Publish(*group);
  }
  BindVisible(selected);
  ScheduleLayout();
  return true;
}
void MultiPageController::Focus(content::WebContents* contents) {
  if (!active_ || binding_ || !contents)
    return;
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab || tab->GetBrowserWindowInterface() != &*owner_->browser_)
    return;
  if (tab == owner_->browser_->GetActiveTabInterface())
    return;
  const auto id = BrowserAdapter::Identity(*tab);
  if (glance_.phase != KR_GLANCE_CLOSED && id == glance_.parent) {
    if (auto* child = Tab(glance_.child))
      child->GetContents()->Focus();
    return;
  }
  const auto weak = weak_factory_.GetWeakPtr();
  owner_->Activate(id);
  if (!weak || glance_.phase == KR_GLANCE_CLOSED)
    return;
  const auto* active = owner_->browser_->GetActiveTabInterface();
  if (!active || BrowserAdapter::Identity(*active) != id) {
    // The unload guard rejected this focus departure. Keep native keyboard
    // focus aligned with Chromium's still-active preview while it confirms.
    if (auto* child = Tab(glance_.child))
      child->GetContents()->Focus();
  }
}
void MultiPageController::ScheduleFocus(content::WebContents* contents) {
  if (!active_ || binding_ || !contents)
    return;
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  auto* active = owner_->browser_->GetActiveTabInterface();
  if (!tab || !active || tab->GetBrowserWindowInterface() != &*owner_->browser_)
    return;
  // RestoreFocus synchronously notifies us while selection is still guarded.
  // It confirms the existing selection and must not activate it a second time.
  pending_focus_ = tab == active ? 0 : BrowserAdapter::Identity(*tab);
  pending_focus_contents_ = pending_focus_ ? contents->GetWeakPtr() : nullptr;
  focus_origin_ = BrowserAdapter::Identity(*active);
  if (!pending_focus_ || focus_scheduled_)
    return;
  focus_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<MultiPageController> self) {
            if (!self)
              return;
            self->focus_scheduled_ = false;
            const auto target = std::exchange(self->pending_focus_, 0);
            auto contents = std::exchange(self->pending_focus_contents_, {});
            auto* active = self->owner_->browser_->GetActiveTabInterface();
            // A later selection supersedes an earlier native focus event.
            if (!target || !contents || !active ||
                BrowserAdapter::Identity(*active) != self->focus_origin_)
              return;
            for (size_t slot = 0; slot < self->bound_tabs_.size(); ++slot) {
              if (self->bound_tabs_[slot] != target ||
                  !self->containers_[slot] ||
                  !self->containers_[slot]->GetVisible())
                continue;
              if (auto* tab = self->Tab(target);
                  tab && tab->GetContents() == contents.get())
                self->Focus(tab->GetContents());
              return;
            }
          },
          weak_factory_.GetWeakPtr()));
}
void MultiPageController::AppendLayout(gfx::Rect available,
                                       views::ProposedLayout& layout) {
  const auto* api = owner_->session_.api();
  const bool fullscreen = ExclusiveAccessManager::From(&*owner_->browser_)
                              ->fullscreen_controller()
                              ->IsTabFullscreen();
  KrSplitGeometry geometry{sizeof(KrSplitGeometry), pane_bounds_.data(),
                           pane_bounds_.size(),     sizeof(KrPaneBounds),
                           divider_bounds_.data(),  divider_bounds_.size(),
                           sizeof(KrDividerBounds)};
  if (visible_) {
    const auto record = visible_->Record();
    const KrSplitLayoutRequest request{
        sizeof(KrSplitLayoutRequest),
        &record,
        {static_cast<float>(available.x()), static_cast<float>(available.y()),
         static_cast<float>(available.width()),
         static_cast<float>(available.height())},
        metrics_.divider_width,
        base::i18n::IsRTL()};
    CHECK_EQ(api->layout_split(api->self, &request, &geometry), KR_OK);
  } else {
    geometry.pane_count = 1;
    geometry.divider_count = 0;
    pane_bounds_[0] = {
        sizeof(KrPaneBounds),
        glance_.parent,
        {static_cast<float>(available.x()), static_cast<float>(available.y()),
         static_cast<float>(available.width()),
         static_cast<float>(available.height())}};
  }
  // Cache the complete geometry before Views applies any child bounds and
  // invokes WebContents or dialog callbacks. No XAML render is involved.
  std::array<gfx::Rect, 5> bounds{};
  std::array<bool, 5> visible{};
  gfx::Rect toolbar_bounds, backdrop_bounds;
  bool show_toolbar = false;
  ContentsContainerView* glance_parent = nullptr;
  gfx::Size parent_size;
  for (size_t pane = 0; pane < geometry.pane_count; ++pane) {
    const auto slot = pane_containers_[pane];
    bounds[slot] = Bounds(pane_bounds_[pane].bounds);
    visible[slot] = true;
  }
  if (glance_.phase != KR_GLANCE_CLOSED && !fullscreen) {
    KrRect parent{};
    for (size_t i = 0; i < geometry.pane_count; ++i)
      if (pane_bounds_[i].tab == glance_.parent)
        parent = pane_bounds_[i].bounds;
    KrGlanceGeometry glance_geometry{.struct_size = sizeof(KrGlanceGeometry)};
    const auto glance_layout =
        glance_controls_ ? api->layout_glance_card : api->layout_glance;
    CHECK_EQ(glance_layout(api->self, parent, &glance_geometry), KR_OK);
    bounds[4] = Bounds(glance_geometry.page);
    visible[4] = true;
    glance_split_button_->SetEnabled(glance_.phase == KR_GLANCE_OPEN &&
                                     (!visible_ || visible_->count < 4));
    if (glance_backdrop_) {
      // Refresh the side only on real layout events, including sidebar moves.
      CHECK_EQ(api->get_multi_page_metrics(api->self, &metrics_), KR_OK);
      glance_controls_->SetSide(metrics_.glance_controls_on_left);
      backdrop_bounds = Bounds(parent);
      gfx::Rect card = bounds[4];
      card.Offset(-backdrop_bounds.x(), -backdrop_bounds.y());
      glance_backdrop_->Update(card, view_->GetBackgroundRadii().upper_left(),
                               owner_->backdrop_->glance_shadow(), metrics_);
      for (size_t i = 0; i < geometry.pane_count; ++i)
        if (pane_bounds_[i].tab == glance_.parent)
          glance_parent = containers_[pane_containers_[i]];
      parent_size = backdrop_bounds.size();
      containers_[4]->SetRoundedCorners(view_->GetBackgroundRadii());
    }
    toolbar_bounds = Bounds(glance_geometry.toolbar);
    show_toolbar = true;
    glance_toolbar_->SetEnabled(glance_.phase == KR_GLANCE_OPEN);
  }
  if (fullscreen) {
    desired_glance_parent_ = nullptr;
    CancelGlanceGenie();
    if (containers_[4] && containers_[4]->layer()) {
      containers_[4]->contents_view()->SetFastResize(
          glance_fast_resize_before_);
      containers_[4]->layer()->GetAnimator()->StopAnimating();
      containers_[4]->SetTransform(gfx::Transform());
      containers_[4]->layer()->SetOpacity(1);
    }
    visible.fill(false);
    visible[selected_container_] = true;
    bounds[selected_container_] = available;
    geometry.divider_count = 0;
  }
  if (visible_) {
    // Each page in a group is its own rounded card, like a single page.
    for (size_t pane = 0; pane < geometry.pane_count; ++pane)
      containers_[pane_containers_[pane]]->SetRoundedCorners(
          view_->GetBackgroundRadii());
  }
  if (outline_) {
    // Zen outlines the selected page only; with Glance open, its parent.
    std::optional<size_t> focused;
    if (visible_ && !fullscreen) {
      for (size_t pane = 0; pane < geometry.pane_count; ++pane) {
        if (glance_.phase != KR_GLANCE_CLOSED
                ? pane_bounds_[pane].tab == glance_.parent
                : pane_containers_[pane] == selected_container_)
          focused = pane;
      }
    }
    gfx::Rect outline;
    if (focused) {
      outline = Bounds(pane_bounds_[*focused].bounds);
      outline.Outset(base::ClampCeil(metrics_.outline_width));
    }
    OutlineChanged();
    layout.child_layouts.emplace_back(outline_.get(), focused.has_value(),
                                      outline);
  }
  if (glance_close_animating_)
    glance_parent = nullptr;
  surface_bounds_ = bounds;
  if (!glance_parent) {
    UpdateGlanceParent(nullptr, {}, false);
  } else if (glance_animated_generation_ != glance_.generation ||
             dimmed_parent_ != glance_parent || dimmed_size_ != parent_size) {
    ScheduleGlancePresentation(glance_parent, parent_size);
  }
  for (size_t i = 0; i < containers_.size(); ++i)
    if (containers_[i])
      layout.child_layouts.emplace_back(containers_[i].get(), visible[i],
                                        bounds[i]);
  if (glance_backdrop_)
    layout.child_layouts.emplace_back(glance_backdrop_.get(), show_toolbar,
                                      backdrop_bounds);
  if (glance_toolbar_)
    layout.child_layouts.emplace_back(glance_toolbar_.get(), show_toolbar,
                                      toolbar_bounds);
  for (size_t i = 0; i < dividers_.size(); ++i) {
    const bool show = i < geometry.divider_count;
    if (dividers_[i]) {
      if (show)
        dividers_[i]->Update(divider_bounds_[i], base::i18n::IsRTL(),
                             visible_->id, visible_->revision);
      layout.child_layouts.emplace_back(
          dividers_[i].get(), show,
          show ? Bounds(divider_bounds_[i].bounds) : gfx::Rect());
    }
  }
  for (size_t slot = 0; slot < pane_controls_.size(); ++slot) {
    if (!pane_controls_[slot])
      continue;
    gfx::Rect control;
    bool show = false;
    if (visible_ && !fullscreen) {
      for (size_t pane = 0; pane < geometry.pane_count; ++pane) {
        if (pane_containers_[pane] == slot) {
          control = Bounds(pane_bounds_[pane].controls);
          pane_controls_[slot]->Update(
              bound_tabs_[slot], visible_->id, visible_->revision,
              *owner_->backdrop_, metrics_,
              view_->GetBackgroundRadii().upper_left(), control.height());
          show = glance_.phase == KR_GLANCE_CLOSED;
        }
      }
    }
    layout.child_layouts.emplace_back(pane_controls_[slot].get(), show,
                                      control);
  }
  if (drop_outline_) {
    const bool current = pane_drag_ && visible_ && !fullscreen &&
                         glance_.phase == KR_GLANCE_CLOSED &&
                         pane_drag_->group == visible_->id &&
                         pane_drag_->revision == visible_->revision;
    if (!current)
      ClearPaneDrag();
    const bool show = current && pane_drag_->target &&
                      *pane_drag_->target < geometry.pane_count;
    drop_outline_->DropStyle(*owner_->backdrop_, metrics_,
                             view_->GetBackgroundRadii().upper_left());
    layout.child_layouts.emplace_back(
        drop_outline_.get(), show,
        show ? Bounds(pane_bounds_[*pane_drag_->target].bounds) : gfx::Rect());
  }
}
gfx::Rect MultiPageController::ActiveBounds() const {
  return active_ ? surface_bounds_[selected_container_] : gfx::Rect();
}
std::optional<uint64_t> MultiPageController::ResizeDivider(
    size_t index,
    float weight,
    bool complete,
    KrSplitGroupId expected_group,
    uint64_t expected_revision) {
  if (!visible_ || index >= visible_->count - 1 ||
      visible_->id != expected_group)
    return std::nullopt;
  const auto id = visible_->id;
  const auto found = groups_.find(id);
  if (found == groups_.end() || found->second.revision != expected_revision)
    return std::nullopt;
  const auto group = found->second;
  if (EditGroup(id,
                {sizeof(KrSplitEdit), KR_SPLIT_RESIZE, group.revision, 0,
                 group.space, index, group.layout, weight},
                complete) != KR_OK)
    return std::nullopt;
  return groups_.at(id).revision;
}

void MultiPageController::ExpandPane(KrTabId id,
                                     KrSplitGroupId group,
                                     uint64_t revision) {
  const auto info = Describe(id);
  if (!info || info->id != group || info->revision != revision)
    return;
  if (DetachMember(id))
    if (auto* tab = Tab(id))
      Focus(tab->GetContents());
}
bool MultiPageController::BeginPaneDrag(KrTabId id,
                                        KrSplitGroupId group,
                                        uint64_t revision) {
  ClearPaneDrag();
  const auto info = Describe(id);
  if (!info || info->id != group || info->revision != revision || !visible_ ||
      visible_->id != group || glance_.phase != KR_GLANCE_CLOSED ||
      !view_->GetWidget() || !view_->GetWidget()->IsActive())
    return false;
  if (!drop_outline_) {
    drop_outline_ = view_->AddChildView(std::make_unique<PaneOutline>(true));
    drop_outline_->SetVisible(false);
  }
  pane_drag_ = PaneDrag{id, group, revision, std::nullopt};
  return true;
}
std::optional<size_t> MultiPageController::PaneDropIndex(
    KrTabId id,
    KrSplitGroupId group,
    uint64_t revision,
    gfx::Point screen) const {
  const auto info = Describe(id);
  if (!info || info->id != group || info->revision != revision || !visible_ ||
      visible_->id != group || glance_.phase != KR_GLANCE_CLOSED ||
      !view_->GetWidget() || !view_->GetWidget()->IsActive())
    return std::nullopt;
  views::View::ConvertPointFromScreen(view_, &screen);
  for (size_t index = 0; index < visible_->count; ++index)
    if (index != info->index &&
        Bounds(pane_bounds_[index].bounds).Contains(screen))
      return index;
  return std::nullopt;
}
bool MultiPageController::UpdatePaneDrag(KrTabId id,
                                         KrSplitGroupId group,
                                         uint64_t revision,
                                         gfx::Point screen) {
  if (!pane_drag_ || pane_drag_->tab != id || pane_drag_->group != group ||
      pane_drag_->revision != revision)
    return false;
  const auto index = PaneDropIndex(id, group, revision, screen);
  if (pane_drag_->target != index) {
    pane_drag_->target = index;
    drop_outline_->DropStyle(*owner_->backdrop_, metrics_,
                             view_->GetBackgroundRadii().upper_left());
    if (index)
      drop_outline_->SetBoundsRect(Bounds(pane_bounds_[*index].bounds));
    drop_outline_->SetVisible(index.has_value());
  }
  return index.has_value();
}
void MultiPageController::ClearPaneDrag() {
  pane_drag_.reset();
  if (drop_outline_)
    drop_outline_->SetVisible(false);
}
void MultiPageController::CancelPaneDrag(KrTabId id,
                                         KrSplitGroupId group,
                                         uint64_t revision) {
  if (pane_drag_ && pane_drag_->tab == id && pane_drag_->group == group &&
      pane_drag_->revision == revision)
    ClearPaneDrag();
}
void MultiPageController::DropPane(KrTabId id,
                                   KrSplitGroupId group,
                                   uint64_t revision,
                                   gfx::Point screen) {
  if (!pane_drag_ || pane_drag_->tab != id || pane_drag_->group != group ||
      pane_drag_->revision != revision)
    return;
  const auto index = PaneDropIndex(id, group, revision, screen);
  const auto info = Describe(id);
  ClearPaneDrag();
  if (index && info &&
      EditGroup(group, {sizeof(KrSplitEdit), KR_SPLIT_MOVE, revision, id,
                        info->space, *index, info->layout, 0.5f}) == KR_OK) {
    if (auto* tab = Tab(id))
      Focus(tab->GetContents());
  }
}

void MultiPageController::PublishGlance() {
  const auto* api = owner_->session_.api();
  CHECK_EQ(api->set_glance(api->self, &glance_), KR_OK);
  ScheduleLayout();
}
void MultiPageController::RequestCloseGlance(KrTabId departure,
                                             bool close_parent) {
  if (glance_.phase != KR_GLANCE_OPEN)
    return;
  const auto* api = owner_->session_.api();
  const KrGlanceEdit edit{sizeof(KrGlanceEdit),
                          KR_GLANCE_REQUEST_CLOSE,
                          glance_.parent,
                          glance_.child,
                          glance_.generation,
                          departure,
                          visible_ ? visible_->count : 1};
  KrGlanceState next{.struct_size = sizeof(KrGlanceState)};
  if (api->reduce_glance(api->self, &glance_, &edit, &next) != KR_OK)
    return;
  glance_ = next;
  close_parent_ = close_parent;
  PublishGlance();
  const auto identity = glance_;
  (void)owner_->session_.Defer(base::BindOnce(
      [](base::WeakPtr<MultiPageController> self, KrGlanceState expected) {
        if (!self || self->glance_.generation != expected.generation ||
            self->glance_.child != expected.child ||
            self->glance_.phase != KR_GLANCE_CLOSING)
          return;
        if (auto* tab = self->Tab(expected.child))
          self->owner_->browser_->GetTabStripModel()->CloseWebContents(
              tab->GetContents(), TabCloseTypes::CLOSE_USER_GESTURE);
      },
      weak_factory_.GetWeakPtr(), identity));
}
bool MultiPageController::BeforeSelection(tabs::TabInterface* target) {
  if (owner_->browser_->GetTabStripModel()->closing_all() ||
      (glance_.phase == KR_GLANCE_CLOSING && !Tab(glance_.child)))
    return true;
  if (!target || glance_.phase == KR_GLANCE_CLOSED ||
      BrowserAdapter::Identity(*target) == glance_.child)
    return true;
  RequestCloseGlance(BrowserAdapter::Identity(*target));
  return false;
}
MultiPageVisibilityChange MultiPageController::SelectionVisibility(
    tabs::TabInterface* target) {
  if (!target || (!active_ && !IsTransientGlance(target->GetContents())))
    return {};
  MultiPageVisibilityChange change{.handled = true};
  const auto next = BrowserAdapter::Identity(*target);
  auto parent = next;
  if (auto* contents = GlanceSessionParent(target->GetContents())) {
    if (auto* tab = tabs::TabInterface::MaybeGetFromContents(contents))
      parent = BrowserAdapter::Identity(*tab);
  }
  const auto* group = GroupFor(parent);
  for (size_t i = 0; i < bound_tabs_.size(); ++i) {
    if (!bound_tabs_[i] || !containers_[i] || !containers_[i]->GetVisible())
      continue;
    const auto id = bound_tabs_[i];
    bool keep = id == next || id == parent;
    if (group)
      keep |= std::ranges::any_of(
          std::span(group->members).first(group->count),
          [id](const KrSplitMember& member) { return member.tab == id; });
    if (!keep) {
      if (auto* tab = Tab(id))
        change.hidden[change.count++] = tab;
    }
  }
  return change;
}
bool MultiPageController::DeferDeparture(base::OnceClosure action) {
  if (glance_.phase != KR_GLANCE_OPEN)
    return false;
  pending_departure_ = std::make_unique<PendingDeparture>();
  pending_departure_->action = std::move(action);
  RequestCloseGlance();
  return false;
}
bool MultiPageController::BeforeWorkspaceChange(KrSpaceId space) {
  if (glance_.phase == KR_GLANCE_CLOSED || space == owner_->active_space_)
    return true;
  return DeferDeparture(base::BindOnce(&BrowserAdapter::SwitchWorkspace,
                                       owner_->weak_factory_.GetWeakPtr(),
                                       space));
}
tabs::TabInterface* MultiPageController::PresentedTab(
    tabs::TabInterface* tab) const {
  return tab && glance_.phase != KR_GLANCE_CLOSED &&
                 BrowserAdapter::Identity(*tab) == glance_.child
             ? Tab(glance_.parent)
             : tab;
}
bool MultiPageController::BeforeTabMove(KrTabId id,
                                        KrSpaceId space,
                                        size_t index) {
  const auto* tab = Tab(id);
  if (!tab || glance_.phase == KR_GLANCE_CLOSED ||
      WorkspaceForTab(tab->GetContents()) == space)
    return true;
  const auto* group = GroupFor(id);
  if (id != glance_.parent && (!group || group != GroupFor(glance_.parent)))
    return true;
  return DeferDeparture(base::BindOnce(&BrowserAdapter::MoveTab,
                                       owner_->weak_factory_.GetWeakPtr(), id,
                                       space, index));
}
bool MultiPageController::BeforeClose(content::WebContents* contents) {
  if (owner_->browser_->GetTabStripModel()->closing_all())
    return true;
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab || glance_.phase == KR_GLANCE_CLOSED)
    return true;
  const auto id = BrowserAdapter::Identity(*tab);
  if (id == glance_.child) {
    if (glance_.phase == KR_GLANCE_CLOSING)
      return true;
    RequestCloseGlance();
    return false;
  }
  if (id == glance_.parent) {
    RequestCloseGlance(0, true);
    return false;
  }
  return true;
}
void MultiPageController::BeforeUnloadResult(content::WebContents* contents,
                                             bool proceed) {
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (proceed || !tab || glance_.phase != KR_GLANCE_CLOSING ||
      BrowserAdapter::Identity(*tab) != glance_.child)
    return;
  const auto* api = owner_->session_.api();
  const KrGlanceEdit edit{sizeof(KrGlanceEdit),
                          KR_GLANCE_CANCEL_CLOSE,
                          glance_.parent,
                          glance_.child,
                          glance_.generation,
                          0,
                          visible_ ? visible_->count : 1};
  KrGlanceState next{.struct_size = sizeof(KrGlanceState)};
  CHECK_EQ(api->reduce_glance(api->self, &glance_, &edit, &next), KR_OK);
  glance_ = next;
  close_parent_ = false;
  glance_close_animating_ = false;
  glance_close_ready_ = false;
  if (containers_[4] && containers_[4]->layer()) {
    containers_[4]->contents_view()->SetFastResize(glance_fast_resize_before_);
    containers_[4]->layer()->GetAnimator()->StopAnimating();
    containers_[4]->SetTransform(gfx::Transform());
    containers_[4]->layer()->SetOpacity(1);
  }
  if (glance_backdrop_) {
    glance_backdrop_->layer()->GetAnimator()->StopAnimating();
    glance_backdrop_->layer()->SetOpacity(1);
  }
  if (glance_toolbar_) {
    glance_toolbar_->layer()->GetAnimator()->StopAnimating();
    glance_toolbar_->layer()->SetOpacity(1);
    glance_toolbar_->SetTransform(gfx::Transform());
    glance_toolbar_->SetEnabled(true);
  }
  pending_departure_.reset();
  PublishGlance();
}
void MultiPageController::Removed(KrTabId id) {
  for (size_t i = 0; i < bound_tabs_.size(); ++i) {
    if (bound_tabs_[i] == id) {
      bound_tabs_[i] = 0;
      if (!active_)
        continue;
      // Selection can rebind Chromium's stock WebView before our removal
      // observer runs. Only detach the page that actually belongs to this id.
      auto* contents = containers_[i]->contents_view()->web_contents();
      auto* tab = contents ? tabs::TabInterface::MaybeGetFromContents(contents)
                           : nullptr;
      if (tab && BrowserAdapter::Identity(*tab) != id) {
        bound_tabs_[i] = BrowserAdapter::Identity(*tab);
        continue;
      }
      if (i == 4)
        containers_[i]->contents_view()->SetFastResize(
            glance_fast_resize_before_);
      containers_[i]->contents_view()->SetWebContents(nullptr);
      containers_[i]->SetVisible(false);
    }
  }
  if (auto* group = GroupFor(id))
    (void)EditGroup(group->id,
                    {sizeof(KrSplitEdit), KR_SPLIT_DETACH, group->revision, id,
                     group->space, 0, group->layout, 0.5f});
  if (glance_.phase != KR_GLANCE_CLOSED && id == glance_.child) {
    const auto previous = glance_;
    glance_close_animating_ = false;
    glance_close_ready_ = false;
    const auto close_parent = std::exchange(close_parent_, false);
    glance_ = {sizeof(KrGlanceState), 0, 0, previous.generation,
               KR_GLANCE_CLOSED,      0};
    PublishGlance();
    (void)owner_->session_.Defer(base::BindOnce(
        [](base::WeakPtr<MultiPageController> self, KrGlanceState expected,
           bool close_parent) {
          if (!self || self->glance_.phase != KR_GLANCE_CLOSED ||
              self->glance_.generation != expected.generation)
            return;
          const auto target =
              expected.departure ? expected.departure : expected.parent;
          auto pending = std::move(self->pending_departure_);
          if (pending && !close_parent && pending->action) {
            self->owner_->Activate(expected.parent);
            if (self && self->glance_.phase == KR_GLANCE_CLOSED &&
                self->glance_.generation == expected.generation)
              std::move(pending->action).Run();
            return;
          }
          if (pending && !close_parent && pending->parent == target &&
              pending->source) {
            auto* parent = self->Tab(target);
            if (parent && parent->GetContents() == pending->source.get()) {
              pending->params->source_contents = pending->source.get();
              self->owner_->Activate(target);
              if (self)
                (void)self->OpenGlance(*pending->params,
                                       std::move(pending->callback));
              return;
            }
          }
          if (close_parent)
            self->owner_->CloseTab(expected.parent);
          else
            self->owner_->Activate(target);
        },
        weak_factory_.GetWeakPtr(), previous, close_parent));
  } else if (glance_.phase != KR_GLANCE_CLOSED && id == glance_.parent) {
    RequestCloseGlance();
  }
  ScheduleLayout();
}
void MultiPageController::Replaced(KrTabId id) {
  if (auto* tab = Tab(id)) {
    for (size_t i = 0; i < bound_tabs_.size(); ++i)
      if (active_ && bound_tabs_[i] == id && containers_[i])
        containers_[i]->contents_view()->SetWebContents(tab->GetContents());
    if (auto* group = GroupFor(id))
      Persist(*group);
    if (glance_.phase != KR_GLANCE_CLOSED &&
        (id == glance_.child || id == glance_.parent)) {
      auto* child = Tab(glance_.child);
      auto* parent = Tab(glance_.parent);
      if (child && parent)
        SetTransientGlance(child->GetContents(), true, parent->GetContents());
    }
  }
}
content::WebContents* MultiPageController::OpenGlance(
    NavigateParams& params,
    base::OnceCallback<void(content::NavigationHandle&)> callback) {
  auto* source = params.source_contents.get();
  auto* parent =
      source ? tabs::TabInterface::MaybeGetFromContents(source) : nullptr;
  if (!parent || parent->GetBrowserWindowInterface() != &*owner_->browser_ ||
      !params.url.SchemeIsHTTPOrHTTPS() || !params.user_gesture)
    return nullptr;
  if (glance_.phase != KR_GLANCE_CLOSED) {
    if (glance_.phase != KR_GLANCE_OPEN)
      return nullptr;
    const auto parent_id = BrowserAdapter::Identity(*parent);
    if (parent_id != glance_.parent && parent_id != glance_.child) {
      // A link in another pane replaces the window's preview only after the
      // old child has passed Chromium's unload confirmation. Preserve the
      // original navigation metadata and validate both identities on resume.
      auto* child = Tab(glance_.child);
      if (!child)
        return nullptr;
      pending_departure_ = std::make_unique<PendingDeparture>();
      pending_departure_->parent = parent_id;
      pending_departure_->source = source->GetWeakPtr();
      pending_departure_->params =
          std::make_unique<NavigateParams>(std::move(params));
      pending_departure_->params->source_contents = nullptr;
      pending_departure_->callback = std::move(callback);
      RequestCloseGlance(parent_id);
      return child->GetContents();
    }
    params.disposition = WindowOpenDisposition::CURRENT_TAB;
    params.source_contents = Tab(glance_.child)->GetContents();
    const auto weak = weak_factory_.GetWeakPtr();
    const auto handle = Navigate(&params);
    if (!weak)
      return nullptr;
    if (handle && callback)
      std::move(callback).Run(*handle);
    if (!weak)
      return nullptr;
    return params.navigated_or_inserted_contents;
  }
  const auto previous_opening =
      std::exchange(opening_parent_, BrowserAdapter::Identity(*parent));
  glance_origin_ = display::Screen::Get()->GetCursorScreenPoint();
  views::View::ConvertPointFromScreen(view_, &glance_origin_);
  params.krypton_glance = true;
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  const auto weak = weak_factory_.GetWeakPtr();
  const auto handle = Navigate(&params);
  if (!weak)
    return nullptr;
  if (params.navigated_or_inserted_contents) {
    (void)owner_->PrepareMultiPageSelection(
        params.navigated_or_inserted_contents);
    PublishGlance();
  }
  opening_parent_ = previous_opening;
  if (handle && callback)
    std::move(callback).Run(*handle);
  if (!weak)
    return nullptr;
  return params.navigated_or_inserted_contents;
}
void MultiPageController::PromoteGlance(bool split) {
  if (glance_.phase != KR_GLANCE_OPEN)
    return;
  auto* child = Tab(glance_.child);
  auto* parent = Tab(glance_.parent);
  if (!child || !parent)
    return;
  auto* group = GroupFor(glance_.parent);
  if (split && group && group->count == 4)
    return;
  const auto previous = glance_;
  const auto* api = owner_->session_.api();
  const KrGlanceEdit edit{
      sizeof(KrGlanceEdit),    split ? KR_GLANCE_SPLIT : KR_GLANCE_EXPAND,
      glance_.parent,          glance_.child,
      glance_.generation,      0,
      group ? group->count : 1};
  KrGlanceState next{.struct_size = sizeof(KrGlanceState)};
  if (api->reduce_glance(api->self, &glance_, &edit, &next) != KR_OK)
    return;
  SetTransientGlance(child->GetContents(), false);
  owner_->AssignWorkspace(*child, WorkspaceForTab(parent->GetContents()));
  glance_ = next;
  PublishGlance();
  if (auto* service = SessionServiceFactory::GetForProfile(
          owner_->browser_->GetProfile())) {
    service->TabRestored(child->GetContents(), false);
    // Promotion does not insert or activate a new tab, so the usual model
    // observer cannot publish its now-persistent index and selection.
    auto* model = owner_->browser_->GetTabStripModel();
    service->SetTabIndexInWindow(
        owner_->browser_->GetSessionID(),
        sessions::SessionTabHelper::IdForTab(child->GetContents()),
        model->GetIndexOfTab(child));
    service->SetSelectedTabInWindow(owner_->browser_->GetSessionID(),
                                    model->active_index());
  }
  if (split && group && !parent->IsPinned()) {
    const auto id = group->id;
    (void)EditGroup(
        id, {sizeof(KrSplitEdit), KR_SPLIT_INSERT, group->revision,
             previous.child, group->space, group->count, group->layout, 0.5f});
  } else if (split && group) {
    // Mixed pinned/unpinned splits use regular copies of saved tabs. Keep the
    // pinned group intact and adopt the promoted child's original WebContents.
    std::array<KrTabId, 4> members{};
    for (size_t i = 0; i < group->count; ++i)
      members[i] = group->members[i].tab;
    members[group->count] = previous.child;
    (void)CreateGroup(std::span(members).first(group->count + 1), group->layout,
                      previous.child);
  } else if (split) {
    const std::array members{previous.parent, previous.child};
    (void)CreateGroup(members, KR_SPLIT_COLUMNS, previous.child);
  }
  (void)PrepareSelection(child->GetContents());
  ScheduleLayout();
}

KrResult MultiPageController::JoinSplit(KrTabId pivot,
                                        KrTabId other,
                                        KrSplitLayout layout) {
  auto* pivot_tab = Tab(pivot);
  auto* other_tab = Tab(other);
  if (!pivot_tab || !other_tab || pivot == other ||
      IsTransientGlance(pivot_tab->GetContents()) ||
      IsTransientGlance(other_tab->GetContents()))
    return KR_ERR_INVALID_ARGUMENT;
  // A page joins whichever of the two is already in a group.
  if (!GroupFor(pivot) && GroupFor(other)) {
    std::swap(pivot, other);
    std::swap(pivot_tab, other_tab);
  }
  const auto* active = owner_->browser_->GetActiveTabInterface();
  const KrTabId focus = active ? BrowserAdapter::Identity(*active) : other;
  auto* group = GroupFor(pivot);
  if (!group) {
    if (pivot_tab->IsSplit() || other_tab->IsSplit()) {
      // Saved/native pairs stay native until a Krypton UI action extends
      // them. Adopt both pages rather than dropping their inactive member.
      std::array<KrTabId, 4> members{};
      size_t count = 0;
      auto* model = owner_->browser_->GetTabStripModel();
      const auto append = [&](tabs::TabInterface* tab) {
        const auto id = BrowserAdapter::Identity(*tab);
        const auto selected = std::span(members).first(count);
        if (std::ranges::find(selected, id) == selected.end()) {
          CHECK_LT(count, members.size());
          members[count++] = id;
        }
      };
      for (auto* tab : {pivot_tab, other_tab}) {
        if (tab->IsSplit()) {
          for (auto* member : model->GetSplitData(*tab->GetSplit())->ListTabs())
            append(member);
        } else {
          append(tab);
        }
      }
      auto selected = std::span(members).first(count);
      std::ranges::sort(selected, {}, [&](KrTabId id) {
        return model->GetIndexOfTab(Tab(id));
      });
      return CreateGroup(selected, layout, focus);
    }
    const std::array pair{pivot, other};
    return CreateGroup(pair, layout, focus);
  }
  if (GroupFor(other) == group)
    return KR_OK;
  if (group->count >= 4)
    return KR_ERR_INVALID_ARGUMENT;
  if (!GroupFor(other) && !pivot_tab->IsPinned() && !other_tab->IsPinned() &&
      !EssentialForTab(other_tab->GetContents())) {
    const auto id = group->id;
    return EditGroup(
        id, {sizeof(KrSplitEdit), KR_SPLIT_INSERT, group->revision, other,
             group->space, group->count, group->layout, 0.5f});
  }
  // Saved tabs and members of another group take a new group's copy and
  // detach rules, as a promoted Glance does.
  std::array<KrTabId, 4> members{};
  for (size_t i = 0; i < group->count; ++i)
    members[i] = group->members[i].tab;
  members[group->count] = other;
  return CreateGroup(std::span(members).first(group->count + 1), group->layout,
                     focus);
}
void MultiPageController::ScheduleGlancePresentation(
    ContentsContainerView* parent,
    gfx::Size size) {
  desired_glance_parent_ = parent;
  desired_glance_parent_size_ = size;
  if (glance_presentation_scheduled_)
    return;
  glance_presentation_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<MultiPageController> self) {
            if (!self)
              return;
            auto* widget = self->view_->GetWidget();
            // CalculateProposedLayout can be speculative. Never transform a
            // native host until real child layout and native visibility have
            // been applied.
            if (widget)
              widget->LayoutRootViewIfNecessary();
            if (!self)
              return;
            self->glance_presentation_scheduled_ = false;
            if (!self->active_ || self->glance_.phase != KR_GLANCE_OPEN ||
                !self->desired_glance_parent_ || !self->containers_[4] ||
                !self->containers_[4]->IsDrawn() ||
                ExclusiveAccessManager::From(&*self->owner_->browser_)
                    ->fullscreen_controller()
                    ->IsTabFullscreen())
              return;
            const auto expected = self->glance_;
            const bool first =
                self->glance_animated_generation_ != expected.generation;
            self->UpdateGlanceParent(self->desired_glance_parent_,
                                     self->desired_glance_parent_size_, true);
            if (!first)
              return;
            self->glance_animated_generation_ = expected.generation;
            if (self->glance_controls_)
              self->glance_controls_->AnimateEntrance();
            auto* page = self->containers_[4]->contents_view();
            page->SetFastResize(true);
            self->AnimateGlanceCard(
                true, base::BindOnce(
                          [](base::WeakPtr<MultiPageController> self,
                             KrGlanceState expected) {
                            if (!self)
                              return;
                            (void)self->owner_->session_.Defer(base::BindOnce(
                                &MultiPageController::FinishGlanceOpen, self,
                                expected));
                          },
                          self->weak_factory_.GetWeakPtr(), expected));
          },
          weak_factory_.GetWeakPtr()));
}

void MultiPageController::FinishGlanceOpen(KrGlanceState expected) {
  if (glance_.generation != expected.generation ||
      glance_.child != expected.child || glance_.phase != KR_GLANCE_OPEN ||
      !containers_[4])
    return;
  auto* card = containers_[4].get();
  card->SetTransform(gfx::Transform());
  card->layer()->SetOpacity(1);
  card->contents_view()->SetFastResize(glance_fast_resize_before_);
  // NativeViewHost positions its window in widget coordinates. Repair its
  // final geometry once after motion; animation ticks never lay out the page.
  card->contents_view()->holder()->InvalidateLayout();
  card->InvalidateLayout();
  if (auto* widget = card->GetWidget())
    widget->LayoutRootViewIfNecessary();
}

void MultiPageController::CancelGlanceGenie() {
  if (auto genie = std::move(glance_genie_))
    genie->Finish();
}

void MultiPageController::AnimateGlanceCard(bool opening,
                                            base::OnceClosure complete) {
  CancelGlanceGenie();
  auto* card = containers_[4].get();
  if (opening) {
    if (card && card->layer()) {
      card->layer()->GetAnimator()->StopAnimating();
      card->SetTransform(gfx::Transform());
      card->layer()->SetOpacity(1);
    }
    if (glance_backdrop_)
      glance_backdrop_->layer()->SetOpacity(1);
  }
  if (!card || metrics_.glance_genie_source_scale_x <= 0 ||
      !RichGlanceMotion(*card) || !card->IsDrawn() || !card->GetWidget() ||
      !card->GetWidget()->IsVisible() || card->GetWidget()->IsMinimized()) {
    if (complete)
      std::move(complete).Run();
    return;
  }
  if (!card->layer()) {
    card->SetPaintToLayer();
    card->layer()->SetFillsBoundsOpaquely(false);
  }
  auto* animator = card->layer()->GetAnimator();
  if (opening)
    animator->StopAnimating();
  else
    animator->AbortAllAnimations();
  card->SetTransform(gfx::Transform());
  const auto bounds = surface_bounds_[4];
  if (bounds.IsEmpty()) {
    if (complete)
      std::move(complete).Run();
    return;
  }
  const auto duration = base::Milliseconds(
      metrics_.glance_motion_ms / (opening ? 1 : metrics_.glance_close_speed));
  if (glance_backdrop_) {
    auto* backdrop_layer = glance_backdrop_->layer();
    backdrop_layer->GetAnimator()->StopAnimating();
    if (opening)
      backdrop_layer->SetOpacity(0);
    ui::ScopedLayerAnimationSettings shadow_settings(
        backdrop_layer->GetAnimator());
    shadow_settings.SetTransitionDuration(duration);
    backdrop_layer->SetOpacity(opening ? 1 : 0);
  }
  if (!opening && glance_toolbar_) {
    ui::ScopedLayerAnimationSettings controls_settings(
        glance_toolbar_->layer()->GetAnimator());
    controls_settings.SetPreemptionStrategy(
        ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
    controls_settings.SetTransitionDuration(duration);
    glance_toolbar_->layer()->SetOpacity(0);
    glance_toolbar_->SetEnabled(false);
  }
  // The lamp's source is where the pointer was when Glance opened.
  glance_genie_ = GlanceGenie::Create(
      *card, {.source = gfx::PointF(glance_origin_ - bounds.OffsetFromOrigin()),
              .source_width = metrics_.glance_genie_source_scale_x,
              .source_depth = metrics_.glance_genie_source_scale_y,
              .stretch_share = metrics_.glance_genie_first_fraction,
              .duration = duration,
              .opening = opening});
  if (!glance_genie_) {
    if (complete)
      std::move(complete).Run();
    return;
  }
  // Completion queues model work, so nothing mutates inside an animation step.
  glance_genie_->Start(std::move(complete));
}

bool MultiPageController::ShouldAnimateGlanceClose(
    content::WebContents* contents) const {
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  auto* card = containers_[4].get();
  return !owner_->browser_->GetTabStripModel()->closing_all() && tab &&
         glance_.phase != KR_GLANCE_CLOSED && !glance_close_ready_ &&
         BrowserAdapter::Identity(*tab) == glance_.child && card &&
         card->GetVisible() && card->GetWidget() &&
         card->GetWidget()->IsVisible() && !card->GetWidget()->IsMinimized() &&
         RichGlanceMotion(*card) && metrics_.glance_genie_source_scale_x > 0 &&
         !ExclusiveAccessManager::From(&*owner_->browser_)
              ->fullscreen_controller()
              ->IsTabFullscreen();
}

bool MultiPageController::DeferGlanceDestruction(
    content::WebContents* contents) {
  auto* model = owner_->browser_->GetTabStripModel();
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (model->closing_all() || !tab || glance_.phase == KR_GLANCE_CLOSED ||
      BrowserAdapter::Identity(*tab) != glance_.child || glance_close_ready_)
    return false;
  if (ExclusiveAccessManager::From(&*owner_->browser_)
          ->fullscreen_controller()
          ->IsTabFullscreen())
    return false;
  if (glance_.phase == KR_GLANCE_OPEN) {
    RequestCloseGlance();
    return true;
  }
  if (glance_close_animating_)
    return true;
  if (!containers_[4] || !containers_[4]->GetVisible() ||
      !RichGlanceMotion(*containers_[4]) ||
      metrics_.glance_genie_source_scale_x <= 0)
    return false;
  glance_close_animating_ = true;
  containers_[4]->contents_view()->SetFastResize(true);
  UpdateGlanceParent(nullptr, {}, true);
  const auto expected = glance_;
  auto contents_weak = contents->GetWeakPtr();
  AnimateGlanceCard(
      false,
      base::BindOnce(
          [](base::WeakPtr<MultiPageController> self, KrGlanceState expected,
             base::WeakPtr<content::WebContents> contents) {
            if (!self)
              return;
            (void)self->owner_->session_.Defer(
                base::BindOnce(&MultiPageController::FinishGlanceClose, self,
                               expected, contents));
          },
          weak_factory_.GetWeakPtr(), expected, contents_weak));
  return true;
}

void MultiPageController::FinishGlanceClose(
    KrGlanceState expected,
    base::WeakPtr<content::WebContents> contents) {
  if (glance_.phase != KR_GLANCE_CLOSING ||
      expected.generation != glance_.generation ||
      expected.child != glance_.child || !contents)
    return;
  auto* tab = Tab(expected.child);
  if (!tab || tab->GetContents() != contents.get())
    return;
  glance_close_ready_ = true;
  owner_->browser_->GetTabStripModel()->CloseWebContents(
      contents.get(), TabCloseTypes::CLOSE_USER_GESTURE);
}

void MultiPageController::UpdateGlanceParent(ContentsContainerView* parent,
                                             gfx::Size size,
                                             bool animate) {
  if (dimmed_parent_ == parent && dimmed_size_ == size)
    return;
  if (dimmed_parent_ && dimmed_parent_ != parent) {
    dimmed_parent_->layer()->GetAnimator()->AbortAllAnimations();
    ui::ScopedLayerAnimationSettings settings(
        dimmed_parent_->layer()->GetAnimator());
    settings.SetTransitionDuration(
        animate && RichGlanceMotion(*dimmed_parent_)
            ? base::Milliseconds(metrics_.glance_motion_ms /
                                 metrics_.glance_close_speed)
            : base::TimeDelta());
    dimmed_parent_->layer()->SetOpacity(1);
    dimmed_parent_->SetTransform(gfx::Transform());
  }
  dimmed_parent_ = parent;
  dimmed_size_ = size;
  if (!parent)
    return;
  if (!parent->layer()) {
    parent->SetPaintToLayer();
    parent->layer()->SetFillsBoundsOpaquely(false);
  }
  if (!containers_[4]->layer()) {
    containers_[4]->SetPaintToLayer();
    containers_[4]->layer()->SetFillsBoundsOpaquely(false);
  }
  ui::ScopedLayerAnimationSettings settings(parent->layer()->GetAnimator());
  settings.SetPreemptionStrategy(
      ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
  settings.SetTransitionDuration(
      animate && RichGlanceMotion(*parent)
          ? base::Milliseconds(metrics_.glance_motion_ms)
          : base::TimeDelta());
  settings.SetTweenType(gfx::Tween::EASE_OUT);
  parent->layer()->SetOpacity(metrics_.glance_parent_opacity);
  parent->SetTransform(CenteredScale(size, metrics_.glance_parent_scale));
}

void MultiPageController::ThemeChanged() {
  OutlineChanged();
  if (glance_controls_)
    glance_controls_->UpdateTheme(*owner_->backdrop_);
  if (glance_backdrop_) {
    glance_backdrop_->ThemeChanged();
    ScheduleLayout();
  }
}
void MultiPageController::OutlineChanged() {
  if (outline_)
    outline_->Update(owner_->backdrop_->pane_outline(), metrics_.outline_width,
                     view_->GetBackgroundRadii().upper_left());
}
KrResult MultiPageController::Command(const KrMultiPageCommand& command) {
  if (!KR_HAS_FIELD(&command, KrMultiPageCommand, url))
    return KR_ERR_INVALID_ARGUMENT;
  switch (command.kind) {
    case KR_MULTI_PAGE_SET_SPLIT_LAYOUT: {
      if (!HasSnapLayouts())
        return KR_ERR_UNSUPPORTED;
      const auto* group = GroupFor(command.tab);
      if (!group)
        return KR_ERR_INVALID_ARGUMENT;
      return EditGroup(
          group->id, {sizeof(KrSplitEdit), KR_SPLIT_SET_LAYOUT, group->revision,
                      command.tab, group->space, 0, command.layout, 0.5f});
    }
    case KR_MULTI_PAGE_CHOOSE_SPLIT_LAYOUT: {
      if (!HasSnapLayouts())
        return KR_ERR_UNSUPPORTED;
      const auto* api = owner_->session_.api();
      return api->show_split_chooser_for_layout(api->self, command.layout);
    }
    case KR_MULTI_PAGE_NEW_SPLIT: {
      if (command.member_count == 0) {
        const auto* api = owner_->session_.api();
        return api->show_split_chooser(api->self);
      }
      if (!command.members || command.member_count < 2 ||
          command.member_count > 4 ||
          command.member_size < offsetof(KrSplitMember, tab) + sizeof(KrTabId))
        return KR_ERR_INVALID_ARGUMENT;
      std::array<KrTabId, 4> ids{};
      const auto* bytes = reinterpret_cast<const std::byte*>(command.members);
      if (command.member_size >
          static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()) /
              command.member_count)
        return KR_ERR_INVALID_ARGUMENT;
      for (size_t i = 0; i < command.member_count; ++i) {
        KrSplitMember member{};
        size_t size = 0;
        std::memcpy(&size, bytes + i * command.member_size, sizeof(size));
        if (size < offsetof(KrSplitMember, tab) + sizeof(KrTabId) ||
            size > command.member_size)
          return KR_ERR_INVALID_ARGUMENT;
        std::memcpy(&member, bytes + i * command.member_size,
                    std::min(size, sizeof(member)));
        ids[i] = member.tab;
      }
      return CreateGroup(std::span(ids).first(command.member_count),
                         command.layout, command.tab);
    }
    case KR_MULTI_PAGE_EDIT_SPLIT:
      if (!command.edit || !KR_HAS_FIELD(command.edit, KrSplitEdit, weight))
        return KR_ERR_INVALID_ARGUMENT;
      return EditGroup(command.group, *command.edit);
    case KR_MULTI_PAGE_CLOSE_GLANCE:
      if (glance_.phase == KR_GLANCE_CLOSED)
        return KR_ERR_INVALID_ARGUMENT;
      RequestCloseGlance();
      return KR_OK;
    case KR_MULTI_PAGE_EXPAND_GLANCE:
    case KR_MULTI_PAGE_SPLIT_GLANCE:
      if (glance_.phase != KR_GLANCE_OPEN ||
          (command.revision && command.revision != glance_.generation))
        return KR_ERR_INVALID_ARGUMENT;
      if (command.kind == KR_MULTI_PAGE_SPLIT_GLANCE) {
        const auto* group = GroupFor(glance_.parent);
        if (group && group->count == 4)
          return KR_ERR_INVALID_ARGUMENT;
      }
      PromoteGlance(command.kind == KR_MULTI_PAGE_SPLIT_GLANCE);
      return KR_OK;
    case KR_MULTI_PAGE_OPEN_GLANCE: {
      auto* tab = Tab(command.tab);
      if (!tab || (!command.url.data && command.url.size))
        return KR_ERR_INVALID_ARGUMENT;
      const GURL url(std::string_view(command.url.data ? command.url.data : "",
                                      command.url.size));
      if (!url.SchemeIsHTTPOrHTTPS())
        return KR_ERR_INVALID_ARGUMENT;
      NavigateParams params(&*owner_->browser_, url, ui::PAGE_TRANSITION_LINK);
      params.source_contents = tab->GetContents();
      params.user_gesture = true;
      return OpenGlance(params) ? KR_OK : KR_ERR_INVALID_ARGUMENT;
    }
  }
  return KR_ERR_INVALID_ARGUMENT;
}
bool MultiPageController::HandleKey(unsigned int key,
                                    bool control,
                                    bool alt,
                                    bool shift) {
  if (pane_drag_ && !control && !alt && key == 0x1b) {
    ClearPaneDrag();
    return true;
  }
  if (shift)
    return false;
  if (glance_.phase != KR_GLANCE_CLOSED && !alt && control && key == 'O') {
    PromoteGlance(false);
    return true;
  }
  if (glance_.phase != KR_GLANCE_CLOSED && !control && !alt && key == 0x1b) {
    RequestCloseGlance();
    return true;
  }
  if (!control || !alt ||
      (key != 'G' && key != 'V' && key != 'H' && key != 'U'))
    return false;
  auto* selected = owner_->browser_->GetActiveTabInterface();
  if (!selected)
    return true;
  const auto active = glance_.phase == KR_GLANCE_CLOSED
                          ? BrowserAdapter::Identity(*selected)
                          : glance_.parent;
  const auto layout = key == 'G'   ? KR_SPLIT_GRID
                      : key == 'H' ? KR_SPLIT_ROWS
                                   : KR_SPLIT_COLUMNS;
  if (auto* group = GroupFor(active)) {
    (void)EditGroup(group->id,
                    {sizeof(KrSplitEdit),
                     key == 'U' ? KR_SPLIT_UNSPLIT : KR_SPLIT_SET_LAYOUT,
                     group->revision, active, group->space, 0, layout, 0.5f});
    return true;
  }
  if (key == 'U')
    return true;
  auto* tab = Tab(active);
  auto* model = owner_->browser_->GetTabStripModel();
  if (!tab)
    return true;
  const int start = model->GetIndexOfTab(tab);
  for (int offset = 1; offset < model->count(); ++offset) {
    auto* next = model->GetTabAtIndex((start + offset) % model->count());
    if (IsTransientGlance(next->GetContents()) ||
        (WorkspaceForTab(next->GetContents()) != owner_->active_space_ &&
         !EssentialForTab(next->GetContents())))
      continue;
    const std::array ids{active, BrowserAdapter::Identity(*next)};
    (void)CreateGroup(ids, layout, active);
    break;
  }
  return true;
}

bool MultiPageController::MoveGroup(KrTabId id, KrSpaceId space) {
  auto* group = GroupFor(id);
  if (!group)
    return false;
  if (group->space == space)
    return true;
  const auto* api = owner_->session_.api();
  auto input = group->Record();
  Group next;
  next.count = 4;
  auto output = next.Record();
  const KrSplitEdit edit{sizeof(KrSplitEdit),
                         KR_SPLIT_SET_WORKSPACE,
                         group->revision,
                         id,
                         space,
                         0,
                         group->layout,
                         0.5f};
  if (api->reduce_split(api->self, &input, &edit, &output) != KR_OK)
    return true;  // handled without partially moving host tabs
  next.Adopt(output);
  const base::AutoReset<bool> editing(&editing_group_, true);
  const auto previous = *group;
  // Publish tab ownership first, then one bounded group update. Module renders
  // coalesce, so no intermediate mixed-workspace group is exposed.
  for (size_t i = 0; i < previous.count; ++i) {
    if (auto* tab = Tab(previous.members[i].tab)) {
      owner_->AssignWorkspace(*tab, space);
      owner_->PublishTab(*tab);
    }
  }
  groups_.at(next.id) = next;
  group = &groups_.at(next.id);
  Publish(*group);
  Persist(*group);
  if (auto* selected = owner_->browser_->GetActiveTabInterface())
    (void)PrepareSelection(selected->GetContents());
  return true;
}
bool MultiPageController::PinGroup(KrTabId id, bool pinned) {
  const auto* group = GroupFor(id);
  if (!group)
    return false;
  const base::AutoReset<bool> editing(&editing_group_, true);
  const auto members = *group;
  auto* model = owner_->browser_->GetTabStripModel();
  for (size_t i = 0; i < members.count; ++i) {
    if (auto* tab = Tab(members.members[i].tab))
      model->SetTabPinned(model->GetIndexOfTab(tab), pinned);
  }
  return true;
}
bool MultiPageController::DetachMember(KrTabId id) {
  const auto* group = GroupFor(id);
  if (!group)
    return false;
  return EditGroup(group->id,
                   {sizeof(KrSplitEdit), KR_SPLIT_DETACH, group->revision, id,
                    group->space, 0, group->layout, 0.5f}) == KR_OK;
}
void MultiPageController::RestoreGroups() {
  // Runtime tab handles are deliberately not persisted. Each tab carries its
  // group id and position; only a complete, consistent set is restored.
  base::flat_map<KrSplitGroupId,
                 std::vector<std::pair<KrTabId, SplitSessionData>>>
      saved;
  auto* model = owner_->browser_->GetTabStripModel();
  for (int index = 0; index < model->count(); ++index) {
    auto* tab = model->GetTabAtIndex(index);
    if (const auto state = SplitSessionForTab(tab->GetContents()))
      saved[state->group].emplace_back(BrowserAdapter::Identity(*tab), *state);
  }
  const auto* api = owner_->session_.api();
  for (const auto& [id, entries] : saved) {
    // Reserve persisted identities even when recovery rejects a group. A new
    // group must not alias metadata from a partial session restored later.
    next_group_ = std::max(
        next_group_,
        id == std::numeric_limits<KrSplitGroupId>::max() ? id : id + 1);
    if (groups_.contains(id) || entries.empty())
      continue;
    const auto& first = entries.front().second;
    bool valid = entries.size() == first.count &&
                 (first.layout <= KR_SPLIT_GRID || HasSnapLayouts());
    std::array<KrSplitMember, 4> members{};
    KrSpaceId space = 0;
    std::optional<bool> pinned;
    for (const auto& [tab_id, state] : entries) {
      auto* tab = Tab(tab_id);
      if (!tab || tab->IsSplit() || state.count != first.count ||
          state.layout != first.layout || state.weights != first.weights ||
          state.index >= first.count || members[state.index].tab ||
          EssentialForTab(tab->GetContents()) ||
          (pinned && *pinned != tab->IsPinned())) {
        valid = false;
        break;
      }
      pinned = tab->IsPinned();
      const auto tab_space = WorkspaceForTab(tab->GetContents());
      if (space && space != tab_space) {
        valid = false;
        break;
      }
      space = tab_space;
      members[state.index] = {sizeof(KrSplitMember), tab_id};
    }
    if (!valid)
      continue;  // all members remain ordinary live tabs
    Group restored;
    restored.count = 4;
    auto output = restored.Record();
    KrTabId selected = members[0].tab;
    if (auto* active = owner_->browser_->GetActiveTabInterface()) {
      const auto active_id = BrowserAdapter::Identity(*active);
      if (std::ranges::any_of(members,
                              [active_id](const KrSplitMember& member) {
                                return member.tab == active_id;
                              }))
        selected = active_id;
    }
    const KrSplitCreateRequest create{sizeof(KrSplitCreateRequest),
                                      id,
                                      space,
                                      members.data(),
                                      first.count,
                                      sizeof(KrSplitMember),
                                      selected,
                                      static_cast<KrSplitLayout>(first.layout)};
    if (api->make_split(api->self, &create, &output) != KR_OK)
      continue;
    restored.Adopt(output);
    for (size_t i = 0; i < 3; ++i)
      restored.weights[i].value = first.weights[i];
    groups_.insert_or_assign(id, restored);
    for (size_t i = 0; i < restored.count; ++i)
      membership_.insert_or_assign(restored.members[i].tab, id);
    Publish(groups_.at(id));
    if (id < std::numeric_limits<KrSplitGroupId>::max())
      next_group_ = std::max(next_group_, id + 1);
  }
  if (auto* selected = owner_->browser_->GetActiveTabInterface())
    (void)PrepareSelection(selected->GetContents());
}

void MultiPageController::ScheduleRestore() {
  if (restore_scheduled_)
    return;
  restore_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<MultiPageController> self) {
                       if (!self)
                         return;
                       self->restore_scheduled_ = false;
                       self->RestoreGroups();
                     },
                     weak_factory_.GetWeakPtr()));
}
bool AllowMultiPageSelection(tabs::TabInterface* current,
                             tabs::TabInterface* target) {
  auto* controller = current ? ControllerFor(current->GetContents()) : nullptr;
  return !controller || controller->BeforeSelection(target);
}
bool AllowMultiPageClose(content::WebContents* contents) {
  auto* controller = ControllerFor(contents);
  return !controller || controller->BeforeClose(contents);
}
bool ShouldAnimateGlanceClose(content::WebContents* contents) {
  const auto* controller = ControllerFor(contents);
  return controller && controller->ShouldAnimateGlanceClose(contents);
}
bool DeferGlanceDestruction(content::WebContents* contents) {
  auto* controller = ControllerFor(contents);
  return controller && controller->DeferGlanceDestruction(contents);
}
void MultiPageBeforeUnloadResult(content::WebContents* contents, bool proceed) {
  if (auto* controller = ControllerFor(contents))
    controller->BeforeUnloadResult(contents, proceed);
}
void ReleaseMultiPageMemberForNativeSplit(tabs::TabInterface* tab) {
  auto* controller = tab ? ControllerFor(tab->GetContents()) : nullptr;
  if (controller)
    (void)controller->DetachMember(
        base::checked_cast<KrTabId>(tab->GetHandle().raw_value()));
}
MultiPageVisibilityChange MultiPageSelectionVisibility(
    tabs::TabInterface* current,
    tabs::TabInterface* target) {
  auto* controller = current ? ControllerFor(current->GetContents()) : nullptr;
  return controller ? controller->SelectionVisibility(target)
                    : MultiPageVisibilityChange{};
}
bool RedirectNativeSplit(tabs::TabInterface* pivot,
                         tabs::TabInterface* other,
                         bool stacked) {
  if (!pivot || !other || pivot == other ||
      !base::FeatureList::IsEnabled(kKryptonMultiPage))
    return false;
  auto* browser = pivot->GetBrowserWindowInterface();
  if (!browser || other->GetBrowserWindowInterface() != browser)
    return false;
  // Native drops encode the requested side through tab-strip insertion order.
  // Preserve that order when creating a new Krypton pair.
  auto* model = browser->GetTabStripModel();
  if (model->GetIndexOfTab(other) < model->GetIndexOfTab(pivot))
    std::swap(pivot, other);
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  return view && view->krypton_ui() &&
         view->krypton_ui()->JoinNativeSplit(
             base::checked_cast<KrTabId>(pivot->GetHandle().raw_value()),
             base::checked_cast<KrTabId>(other->GetHandle().raw_value()),
             stacked ? KR_SPLIT_ROWS : KR_SPLIT_COLUMNS);
}
bool MultiPageSplitFull(tabs::TabInterface* tab) {
  const auto* controller = tab ? ControllerFor(tab->GetContents()) : nullptr;
  const auto group = controller
                         ? controller->Describe(base::checked_cast<KrTabId>(
                               tab->GetHandle().raw_value()))
                         : std::nullopt;
  return group && group->count >= 4;
}
bool ShowMultiPageSplitChooser(BrowserWindowInterface* browser) {
  if (!browser || !base::FeatureList::IsEnabled(kKryptonMultiPage))
    return false;
  auto* view = BrowserView::GetBrowserViewForBrowser(browser);
  return view && view->krypton_ui() && view->krypton_ui()->ShowSplitChooser();
}
void SetMultiPageSplitLayout(content::WebContents* source, uint32_t layout) {
  auto* tab = tabs::TabInterface::MaybeGetFromContents(source);
  auto* browser = tab ? tab->GetBrowserWindowInterface() : nullptr;
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  auto* adapter = view ? view->krypton_ui() : nullptr;
  if (adapter && adapter->SnapLayoutsEnabled())
    (void)adapter->MultiPageCommand(
        {.struct_size = sizeof(KrMultiPageCommand),
         .kind = KR_MULTI_PAGE_SET_SPLIT_LAYOUT,
         .tab = base::checked_cast<KrTabId>(tab->GetHandle().raw_value()),
         .layout = static_cast<KrSplitLayout>(layout)});
}
bool PinMultiPageGroup(tabs::TabInterface* tab, bool pinned) {
  auto* controller = tab ? ControllerFor(tab->GetContents()) : nullptr;
  return controller && !controller->editing_group() &&
         controller->PinGroup(
             base::checked_cast<KrTabId>(tab->GetHandle().raw_value()), pinned);
}
}  // namespace krypton_ui
