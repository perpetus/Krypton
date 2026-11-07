// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/control_anchors.h"

#include <windows.h>

#include <utility>

#include "base/check.h"
#include "chrome/browser/ui/krypton_ui/window_geometry.h"
#include "ui/display/win/screen_win.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_tracker.h"
#include "ui/views/win/hwnd_util.h"

namespace krypton_ui {
namespace {

void RemoveAnchor(views::ViewTracker& tracker) {
  if (tracker.view() && tracker.view()->parent()) {
    tracker.view()->parent()->RemoveChildViewT(tracker.view());
  }
}

}  // namespace

ControlAnchors::ControlAnchors(views::View& host) : host_(&host) {
  CHECK(host.GetProperty(views::kElementIdentifierKey));
  observation_.Observe(&host);
}

ControlAnchors::~ControlAnchors() {
  // Removing an anchor can close a bubble and reenter browser teardown.
  weak_factory_.InvalidateWeakPtrs();
  observation_.Reset();
  host_ = nullptr;
  auto anchors = std::move(tracked_);
  for (const auto& [key, anchor] : anchors) {
    if (anchor.view) {
      RemoveAnchor(*anchor.view);
    }
  }
}

void ControlAnchors::Update(uint32_t kind,
                            uint64_t id,
                            KrRect bounds,
                            bool visible) {
  if (bounds_.Update(kind, id, bounds, visible)) {
    const auto key = *AnchorKey::FromAbi(kind, id);
    // Most tab anchors have no native-service consumer. Their updates must
    // remain data-only, without one set of HWND queries per tab on resize.
    if (tracked_.contains(key)) {
      Synchronize(key, ReadHostGeometry());
    }
  }
}

void ControlAnchors::Track(AnchorKey key, ui::ElementIdentifier identifier) {
  CHECK(identifier);
  if (!host_) {
    return;
  }
  const auto [found, inserted] = tracked_.try_emplace(key);
  if (!inserted) {
    CHECK_EQ(found->second.identifier, identifier);
    return;
  }
  found->second.identifier = identifier;
  Synchronize(key, ReadHostGeometry());
}

void ControlAnchors::Untrack(AnchorKey key) {
  const auto found = tracked_.find(key);
  if (found == tracked_.end()) {
    return;
  }
  auto view = std::move(found->second.view);
  tracked_.erase(found);
  if (view) {
    // Remove the registration before notifying native bubbles. A callback may
    // register the same key again or destroy the entire host.
    RemoveAnchor(*view);
  }
}

views::BubbleAnchor ControlAnchors::GetAnchor(AnchorKey key) const {
  if (!host_ || !host_->IsDrawn()) {
    return {};
  }
  const auto found = tracked_.find(key);
  if (found == tracked_.end() || !found->second.view ||
      !found->second.view->view() || !found->second.view->view()->IsDrawn()) {
    return {};
  }
  const auto geometry = ReadHostGeometry();
  const auto bounds = bounds_.Get(key);
  if (!geometry || !bounds ||
      !ResolveAnchorBounds(*bounds, geometry->client,
                           geometry->client_origin)) {
    return {};
  }
  return views::BubbleAnchor(found->second.view->view());
}

std::optional<ControlAnchors::HostGeometry> ControlAnchors::ReadHostGeometry()
    const {
  if (!host_ || !host_->GetWidget()) {
    return std::nullopt;
  }
  const HWND window = views::HWNDForView(host_);
  RECT client{};
  POINT origin{};
  if (!window || !::GetClientRect(window, &client) ||
      !::ClientToScreen(window, &origin)) {
    return std::nullopt;
  }
  const auto state =
      MakeWindowState(client.right - client.left, client.bottom - client.top,
                      ::GetDpiForWindow(window), 0);
  if (!state) {
    return std::nullopt;
  }
  const auto screen_origin =
      display::win::GetScreenWin()
          ->ScreenToDIPRect(window, gfx::Rect(origin.x, origin.y, 0, 0))
          .origin();
  const auto local_origin =
      views::View::ConvertPointFromScreen(host_, screen_origin);
  return HostGeometry{
      gfx::RectF(0, 0, state->client.width, state->client.height),
      gfx::PointF(local_origin)};
}

void ControlAnchors::Synchronize(AnchorKey key,
                                 const std::optional<HostGeometry>& geometry) {
  const auto found = tracked_.find(key);
  if (found == tracked_.end()) {
    return;
  }
  const auto revision = ++found->second.revision;
  if (found->second.creating) {
    return;
  }
  const auto bounds = bounds_.Get(key);
  const auto local = geometry && bounds
                         ? ResolveAnchorBounds(*bounds, geometry->client,
                                               geometry->client_origin)
                         : std::nullopt;
  auto weak = weak_factory_.GetWeakPtr();
  if (!local) {
    if (found->second.view && found->second.view->view()) {
      found->second.view->view()->SetVisible(false);
    }
    return;
  }
  if (!found->second.view || !found->second.view->view()) {
    // Hierarchy and geometry changes notify Views/ElementTracker observers.
    found->second.creating = true;
    auto view = std::make_unique<views::View>();
    view->SetVisible(false);
    view->SetCanProcessEventsWithinSubtree(false);
    view->GetViewAccessibility().SetIsIgnored(true);
    view->SetProperty(views::kElementIdentifierKey, found->second.identifier);
    auto tracker = std::make_unique<views::ViewTracker>(view.get());
    host_->AddChildView(std::move(view));
    if (!weak) {
      RemoveAnchor(*tracker);
      return;
    }
    // A notification may have registered another control and relocated the map.
    const auto current = tracked_.find(key);
    if (current == tracked_.end() || !current->second.creating) {
      RemoveAnchor(*tracker);
      return;
    }
    current->second.creating = false;
    current->second.view = std::move(tracker);
    Synchronize(key, ReadHostGeometry());
    return;
  }
  auto* view = tracked_.at(key).view->view();
  auto physical_bounds = *local;
  // Child positions are mirrored by Views under RTL; native module positions
  // have already been resolved, so compensate exactly once when placing them.
  physical_bounds.set_x(host_->GetMirroredXForRect(physical_bounds));
  view->SetBoundsRect(physical_bounds);
  if (weak && tracked_.contains(key) && tracked_.at(key).view &&
      tracked_.at(key).view->view() == view &&
      tracked_.at(key).revision == revision) {
    view->SetVisible(true);
  }
}

void ControlAnchors::RefreshGeometry() {
  const auto revision = ++geometry_revision_;
  const auto geometry = ReadHostGeometry();
  auto weak = weak_factory_.GetWeakPtr();
  // Track() may insert during observer callbacks, invalidating map iterators.
  std::optional<AnchorKey> previous;
  while (weak && geometry_revision_ == revision) {
    const auto next =
        previous ? tracked_.upper_bound(*previous) : tracked_.begin();
    if (next == tracked_.end()) {
      return;
    }
    previous = next->first;
    Synchronize(*previous, geometry);
  }
}

void ControlAnchors::OnViewBoundsChanged(views::View* observed_view) {
  RefreshGeometry();
}

void ControlAnchors::OnViewAddedToWidget(views::View* observed_view) {
  RefreshGeometry();
}

void ControlAnchors::OnViewHierarchyWillBeDeleted(views::View* observed_view) {
  observation_.Reset();
  host_ = nullptr;
  // The host still owns its children and will delete them after this callback.
  tracked_.clear();
}

}  // namespace krypton_ui
