// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_CONTROL_ANCHORS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_CONTROL_ANCHORS_H_

#include <memory>
#include <optional>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/krypton_ui/anchor_geometry.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/views/bubble/bubble_anchor.h"
#include "ui/views/view_observer.h"

namespace views {
class View;
class ViewTracker;
}  // namespace views

namespace krypton_ui {

// Exposes native control positions to Chromium bubbles and ElementTracker.
// The identified host must be attached to a Widget before Track() is called.
// Only requested controls get invisible child Views; 2,000 tab rectangles do
// not create 2,000 Views. Views owns bubble repositioning and anchor lifetime.
// These anchors never paint, receive input or expose duplicate accessibility.
class ControlAnchors final : public views::ViewObserver {
 public:
  explicit ControlAnchors(views::View& host);
  ~ControlAnchors() override;

  ControlAnchors(const ControlAnchors&) = delete;
  ControlAnchors& operator=(const ControlAnchors&) = delete;

  void Update(uint32_t kind, uint64_t id, KrRect bounds, bool visible);
  void Track(AnchorKey key, ui::ElementIdentifier identifier);
  // Release transient/tab anchors when their service or tab goes away. Hiding
  // compact chrome alone keeps the same anchor View for its next reveal.
  void Untrack(AnchorKey key);
  [[nodiscard]] views::BubbleAnchor GetAnchor(AnchorKey key) const;
  [[nodiscard]] std::optional<gfx::RectF> GetBounds(AnchorKey key) const {
    return bounds_.Get(key);
  }

  // Called when the HWND's DPI/client origin changes. Ordinary host movement,
  // visibility and destruction are also tracked by the Views infrastructure.
  void RefreshGeometry();

 private:
  struct TrackedAnchor {
    ui::ElementIdentifier identifier;
    // The View belongs to the host hierarchy. A tracker also survives reentrant
    // removal of the host or an anchor during a bubble notification.
    std::unique_ptr<views::ViewTracker> view;
    uint64_t revision = 0;
    bool creating = false;
  };
  struct HostGeometry {
    gfx::RectF client;
    gfx::PointF client_origin;
  };

  [[nodiscard]] std::optional<HostGeometry> ReadHostGeometry() const;
  void Synchronize(AnchorKey key, const std::optional<HostGeometry>& geometry);

  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewAddedToWidget(views::View* observed_view) override;
  void OnViewHierarchyWillBeDeleted(views::View* observed_view) override;

  raw_ptr<views::View> host_;
  AnchorGeometry bounds_;
  base::flat_map<AnchorKey, TrackedAnchor> tracked_;
  uint64_t geometry_revision_ = 0;
  base::ScopedObservation<views::View, views::ViewObserver> observation_{this};
  base::WeakPtrFactory<ControlAnchors> weak_factory_{this};
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_CONTROL_ANCHORS_H_
