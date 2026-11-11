// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_CONTROLLER_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MULTI_PAGE_CONTROLLER_H_

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "base/callback_list.h"
#include "base/containers/flat_map.h"
#include "base/feature_list.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/krypton_ui/multi_page_commands.h"
#include "chrome/browser/ui/krypton_ui/multi_page_features.h"
#include "krypton_ui/abi.h"
#include "ui/views/layout/proposed_layout.h"

class BrowserWindowInterface;
class ContentsContainerView;
class ContentsWebView;
class MultiContentsView;
struct NavigateParams;
namespace gfx {
class Point;
}
namespace content {
class WebContents;
class NavigationHandle;
}  // namespace content
namespace tabs {
class TabInterface;
}
namespace views {
class View;
}
namespace views {
class LabelButton;
class Button;
}  // namespace views
namespace krypton_ui {
class BrowserAdapter;
class PaneDivider;
class PaneHeader;
class PaneOutline;
class MultiPageMenuModel;
class GlanceControlsView;
class GlanceBackdrop;
class GlanceGenie;

struct MultiPageVisibilityChange {
  bool handled = false;
  std::array<tabs::TabInterface*, 5> hidden{};
  size_t count = 0;
};

// Owns host tab identities and live surfaces, not layout rules. Bounded edits
// and geometry are evaluated synchronously by the optional shared ABI slots.
// Created only by an explicit feature action or valid session restoration.
class MultiPageController {
 public:
  explicit MultiPageController(BrowserAdapter& owner);
  ~MultiPageController();
  MultiPageController(const MultiPageController&) = delete;
  MultiPageController& operator=(const MultiPageController&) = delete;
  [[nodiscard]] static bool HasCompleteModule(const KrShellUI* api);
  [[nodiscard]] bool HasSnapLayouts() const;
  [[nodiscard]] bool editing_group() const { return editing_group_; }
  struct GroupInfo {
    KrSplitGroupId id;
    KrSpaceId space;
    uint64_t revision;
    KrSplitLayout layout;
    size_t count;
    size_t index;
  };
  [[nodiscard]] std::optional<GroupInfo> Describe(KrTabId tab) const;
  [[nodiscard]] KrResult Command(const KrMultiPageCommand& command);
  // Adds `other` to `pivot`'s group, or pairs them. A full group leaves
  // `other` an ordinary tab.
  [[nodiscard]] KrResult JoinSplit(KrTabId pivot,
                                   KrTabId other,
                                   KrSplitLayout layout);
  // The module resolved a new outline colour for the focused page.
  void OutlineChanged();
  void ExpandPane(KrTabId tab, KrSplitGroupId group, uint64_t revision);
  bool BeginPaneDrag(KrTabId tab, KrSplitGroupId group, uint64_t revision);
  bool UpdatePaneDrag(KrTabId tab,
                      KrSplitGroupId group,
                      uint64_t revision,
                      gfx::Point screen);
  void CancelPaneDrag(KrTabId tab, KrSplitGroupId group, uint64_t revision);
  void DropPane(KrTabId tab,
                KrSplitGroupId group,
                uint64_t revision,
                gfx::Point screen);
  void ThemeChanged();
  // Called only after Chromium has completed or waived its unload check.
  [[nodiscard]] bool DeferGlanceDestruction(content::WebContents* contents);
  [[nodiscard]] bool ShouldAnimateGlanceClose(
      content::WebContents* contents) const;
  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] ContentsContainerView* ActiveContainer() const;
  [[nodiscard]] ContentsContainerView* InactiveContainer() const;
  [[nodiscard]] bool PrepareSelection(content::WebContents* contents);
  [[nodiscard]] bool BeforeSelection(tabs::TabInterface* target);
  [[nodiscard]] bool BeforeWorkspaceChange(KrSpaceId space);
  [[nodiscard]] bool BeforeTabMove(KrTabId tab, KrSpaceId space, size_t index);
  [[nodiscard]] tabs::TabInterface* PresentedTab(tabs::TabInterface* tab) const;
  [[nodiscard]] MultiPageVisibilityChange SelectionVisibility(
      tabs::TabInterface* target);
  [[nodiscard]] bool BeforeClose(content::WebContents* contents);
  void BeforeUnloadResult(content::WebContents* contents, bool proceed);
  void Removed(KrTabId id);
  void Replaced(KrTabId id);
  void Focus(content::WebContents* contents);
  // Native focus notifications can arrive inside TabStripModel mutations.
  // Coalesce them and activate only after the mutation has completed.
  void ScheduleFocus(content::WebContents* contents);
  void AppendLayout(gfx::Rect available, views::ProposedLayout& layout);
  [[nodiscard]] std::optional<uint64_t> ResizeDivider(size_t index,
                                                      float weight,
                                                      bool complete,
                                                      KrSplitGroupId group,
                                                      uint64_t revision);
  [[nodiscard]] bool HandleKey(unsigned int key,
                               bool control,
                               bool alt,
                               bool shift);
  [[nodiscard]] content::WebContents* OpenGlance(
      NavigateParams& params,
      base::OnceCallback<void(content::NavigationHandle&)> callback = {});
  void RestoreGroups();
  void ScheduleRestore();
  [[nodiscard]] bool MoveGroup(KrTabId tab, KrSpaceId space);
  [[nodiscard]] bool PinGroup(KrTabId tab, bool pinned);
  [[nodiscard]] bool DetachMember(KrTabId tab);
  [[nodiscard]] gfx::Rect ActiveBounds() const;

 private:
  struct Group {
    KrSplitGroupId id = 0;
    KrSpaceId space = 0;
    uint64_t revision = 1;
    KrSplitLayout layout = KR_SPLIT_COLUMNS;
    size_t count = 0;
    KrTabId selected = 0;
    std::array<KrSplitMember, 4> members{};
    std::array<KrSplitWeight, 3> weights{};
    Group();
    [[nodiscard]] KrSplitGroup Record();
    void Adopt(const KrSplitGroup& record);
  };

  struct PendingDeparture {
    KrTabId parent = 0;
    base::WeakPtr<content::WebContents> source;
    std::unique_ptr<NavigateParams> params;
    base::OnceCallback<void(content::NavigationHandle&)> callback;
    base::OnceClosure action;
  };
  [[nodiscard]] Group* GroupFor(KrTabId tab);
  [[nodiscard]] KrResult CreateGroup(std::span<const KrTabId> tabs,
                                     KrSplitLayout layout,
                                     KrTabId active);
  [[nodiscard]] KrResult EditGroup(KrSplitGroupId id,
                                   const KrSplitEdit& edit,
                                   bool persist = true);
  void Publish(Group& group);
  void Persist(Group& group);
  void Dissolve(KrSplitGroupId id);
  void RequestCloseGlance(KrTabId departure = 0, bool close_parent = false);
  bool DeferDeparture(base::OnceClosure action);
  void PromoteGlance(bool split);
  void PublishGlance();
  void BindVisible(tabs::TabInterface* selected);
  void EnsureChrome();
  void ClearPaneDrag();
  std::optional<size_t> PaneDropIndex(KrTabId tab,
                                      KrSplitGroupId group,
                                      uint64_t revision,
                                      gfx::Point screen) const;
  void ScheduleGlancePresentation(ContentsContainerView* parent,
                                  gfx::Size size);
  void FinishGlanceOpen(KrGlanceState expected);
  void AnimateGlanceCard(bool opening, base::OnceClosure complete = {});
  // Ends a running magic lamp in its final state and runs its completion.
  void CancelGlanceGenie();
  void FinishGlanceClose(KrGlanceState expected,
                         base::WeakPtr<content::WebContents> contents);
  void UpdateGlanceParent(ContentsContainerView* parent,
                          gfx::Size size,
                          bool animate);
  void OpenPaneMenu(size_t slot);
  void ReleaseHidden();
  void ScheduleLayout();
  [[nodiscard]] ContentsContainerView* EnsureContainer(size_t index);
  [[nodiscard]] tabs::TabInterface* Tab(KrTabId id) const;
  const raw_ref<BrowserAdapter> owner_;
  const raw_ptr<MultiContentsView> view_;
  base::flat_map<KrSplitGroupId, Group> groups_;
  base::flat_map<KrTabId, KrSplitGroupId> membership_;
  std::array<raw_ptr<ContentsContainerView>, 5> containers_{};
  std::array<KrTabId, 5> bound_tabs_{};
  std::array<size_t, 4> pane_containers_{};
  std::array<raw_ptr<PaneDivider>, 3> dividers_{};
  raw_ptr<PaneOutline> outline_ = nullptr;
  raw_ptr<PaneOutline> drop_outline_ = nullptr;
  struct PaneDrag {
    KrTabId tab;
    KrSplitGroupId group;
    uint64_t revision;
    std::optional<size_t> target;
  };
  std::optional<PaneDrag> pane_drag_;
  std::array<raw_ptr<PaneHeader>, 4> pane_controls_{};
  std::array<std::unique_ptr<MultiPageMenuModel>, 4> pane_menus_;
  std::array<base::ScopedClosureRunner, 4> pane_menu_leases_;
  std::array<gfx::Rect, 5> surface_bounds_{};
  std::array<KrPaneBounds, 4> pane_bounds_{};
  std::array<KrDividerBounds, 3> divider_bounds_{};
  KrGlanceState glance_{.struct_size = sizeof(KrGlanceState)};
  KrMultiPageMetrics metrics_{.struct_size = sizeof(KrMultiPageMetrics)};
  std::optional<Group> visible_;
  size_t selected_container_ = 0;
  size_t pane_count_ = 0;
  bool active_ = false, binding_ = false, layout_scheduled_ = false;
  bool editing_group_ = false;
  bool restore_scheduled_ = false;
  bool close_parent_ = false;
  KrTabId opening_parent_ = 0;
  KrTabId pending_focus_ = 0;
  KrTabId focus_origin_ = 0;
  base::WeakPtr<content::WebContents> pending_focus_contents_;
  bool focus_scheduled_ = false;
  std::unique_ptr<PendingDeparture> pending_departure_;
  KrSplitGroupId next_group_ = 1;
  raw_ptr<views::View> glance_toolbar_ = nullptr;
  raw_ptr<GlanceControlsView> glance_controls_ = nullptr;
  raw_ptr<GlanceBackdrop> glance_backdrop_ = nullptr;
  raw_ptr<views::Button> glance_split_button_ = nullptr;
  raw_ptr<ContentsContainerView> dimmed_parent_ = nullptr;
  gfx::Size dimmed_size_;
  gfx::Point glance_origin_;
  uint64_t glance_animated_generation_ = 0;
  bool glance_close_animating_ = false;
  bool glance_close_ready_ = false;
  bool glance_presentation_scheduled_ = false;
  bool glance_fast_resize_before_ = false;
  raw_ptr<ContentsContainerView> desired_glance_parent_ = nullptr;
  gfx::Size desired_glance_parent_size_;
  std::unique_ptr<GlanceGenie> glance_genie_;
  base::WeakPtrFactory<MultiPageController> weak_factory_{this};
};

// Small guarded hooks used by Chromium patches. All have a direct no-op path
// when KryptonUI, its controller, or the feature gate is absent.
[[nodiscard]] bool AllowMultiPageSelection(tabs::TabInterface* current,
                                           tabs::TabInterface* target);
[[nodiscard]] bool AllowMultiPageClose(content::WebContents* contents);
void MultiPageBeforeUnloadResult(content::WebContents* contents, bool proceed);
[[nodiscard]] bool DeferGlanceDestruction(content::WebContents* contents);
[[nodiscard]] bool ShouldAnimateGlanceClose(content::WebContents* contents);
void ReleaseMultiPageMemberForNativeSplit(tabs::TabInterface* tab);
[[nodiscard]] MultiPageVisibilityChange MultiPageSelectionVisibility(
    tabs::TabInterface* current,
    tabs::TabInterface* target);
[[nodiscard]] bool PinMultiPageGroup(tabs::TabInterface* tab, bool pinned);
}  // namespace krypton_ui
#endif
