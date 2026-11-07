// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_TOOLBAR_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_TOOLBAR_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/krypton_ui/menu_model_snapshot.h"
#include "chrome/browser/ui/toolbar/pinned_toolbar/pinned_toolbar_actions_model.h"
#include "chrome/browser/ui/views/frame/app_menu_button_observer.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/browser/ui/views/toolbar/app_menu_control.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions.h"
#include "chrome/browser/ui/views/toolbar/reload_control.h"
#include "krypton_ui/abi.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/accessible_pane_view.h"

class BrowserWindowInterface;
class CommandUpdater;
namespace ui {
class AcceleratorProvider;
}

namespace krypton_ui {
class ControlAnchors;
class WindowSession;

// ToolbarButtonProvider and PinnedToolbarActions both declare
// GetDownloadButton() with unrelated return types, so one class cannot
// override both. Each answers in its own base: KryptonUI draws downloads
// natively, so neither has a Views download button.
class NativeToolbarButtonProvider : public ToolbarButtonProvider {
 public:
  ToolbarButton* GetDownloadButton() final;
};
class NativePinnedToolbarActions : public PinnedToolbarActions {
 public:
  DownloadButton* GetDownloadButton() final;
};

// A nonpainting focus/service pane. Native controls remain in KryptonUI;
// Chromium retains its command, action, menu and bubble implementations.
class NativeToolbar final : public views::AccessiblePaneView,
                            public NativeToolbarButtonProvider,
                            public AppMenuControl,
                            public ReloadControl,
                            public NativePinnedToolbarActions,
                            public PinnedToolbarActionsModel::Observer {
  METADATA_HEADER(NativeToolbar, views::AccessiblePaneView)
 public:
  NativeToolbar(BrowserWindowInterface& browser,
                CommandUpdater& commands,
                ui::AcceleratorProvider& accelerators,
                WindowSession& session,
                ControlAnchors& anchors,
                MenuModelSnapshot::PublishImage images,
                base::RepeatingCallback<void(KrImageId)> release_image,
                base::RepeatingClosure changed);
  ~NativeToolbar() override;
  NativeToolbar(const NativeToolbar&) = delete;
  NativeToolbar& operator=(const NativeToolbar&) = delete;

  void Initialize();
  void SetExtensionsContainer(ExtensionsContainerViews* container) {
    extensions_container_ = container;
  }
  [[nodiscard]] base::expected<void, std::string> PresentAppMenu(
      uint32_t request);
  [[nodiscard]] base::OnceClosure CaptureMenuCommand(uint32_t request,
                                                     int32_t command);
  void OnNativeMenuClosed(uint32_t request);
  void OnAnchorsChanged();
  static KrActionId ActionIdentity(actions::ActionId id);
  const std::vector<actions::ActionId>& PoppedOutActionIds() const {
    return popped_out_;
  }

  ExtensionsContainerViews* GetExtensionsContainerViews() override;
  PinnedToolbarActions* GetPinnedToolbarActions() override;
  gfx::Size GetToolbarButtonSize() const override;
  views::BubbleAnchor GetDefaultExtensionDialogAnchor() override;
  page_actions::PageActionViewInterface* GetPageActionViewInterface(
      actions::ActionId id) override;
  AppMenuControl* GetAppMenuControl() override;
  gfx::Rect GetFindBarBoundingBox(int contents_bottom) override;
  void FocusToolbar() override;
  void RequestFocus() override;
  bool SetPaneFocusAndFocusDefault() override;
  views::AccessiblePaneView* GetAsAccessiblePaneView() override;
  views::BubbleAnchor GetBubbleAnchor(
      std::optional<actions::ActionId> id) override;
  views::BubbleAnchor GetPageActionBubbleAnchor(actions::ActionId id) override;
  void ZoomChangedForActiveTab(bool can_show_bubble) override;
  AvatarToolbarButtonInterface* GetAvatarToolbarButtonInterface() override;
  ToolbarButton* GetBackButton() override;
  ReloadControl* GetReloadButton() override;
  WebUIToolbarWebView* GetWebUIToolbarViewForTesting() override;

  views::BubbleAnchor GetAnchor() override;
  bool IsDrawn() const override;
  bool IsMenuShowing() const override;
  views::DialogDelegate* GetDialogDelegate() override;
  void CloseMenu() override;
  void ShowMenu() override;
  void ShowMenuWithFlags(int run_types) override;
  views::MenuItemView* GetRootMenuItemViewForTesting() override;
  void ExecuteCommandForTesting(int command_id, int mouse_event_flags) override;
  void SetMenuTimerForTesting(base::ElapsedTimer timer) override;
  void AddObserver(AppMenuButtonObserver* observer) override;
  void RemoveObserver(AppMenuButtonObserver* observer) override;
  bool HasFocus() const override;
  void Focus(views::AccessiblePaneView* pane) override;
  void SetTypeAndSeverity(
      AppMenuIconController::TypeAndSeverity state) override;
  void SetIsMaximizedOrFullscreen(bool maximized) override;
  views::View* GetFocusablePaneView() override;

  void ChangeMode(Mode mode, bool force) override;
  bool GetDevToolsStatusForTesting() const override;
  void SetDevToolsStatus(bool connected) override;
  bool IsCommandIdChecked(int command) const override;
  bool IsCommandIdEnabled(int command) const override;
  bool IsCommandIdVisible(int command) const override;
  bool GetAcceleratorForCommandId(int command,
                                  ui::Accelerator* accelerator) const override;
  void ExecuteCommand(int command, int event_flags) override;

  actions::ActionItem* GetActionItemFor(actions::ActionId id) override;
  const std::vector<actions::ActionId>& PinnedActionIds() const override;
  bool IsOverflowed(actions::ActionId id) override;
  views::View* GetContainerView() override;
  bool ShouldAnyButtonsOverflow(gfx::Size available) const override;
  void UpdateActionState(actions::ActionId id, bool active) override;
  void ShowActionEphemerallyInToolbar(actions::ActionId id, bool show) override;
  bool IsActionPinned(actions::ActionId id) override;
  bool IsActionPoppedOut(actions::ActionId id) override;
  bool IsActionPinnedOrPoppedOut(actions::ActionId id) override;
  bool IsActionHighlighted(actions::ActionId id) override;
  void PostOrQueueActionAfterAnimation(base::OnceClosure action) override;
  views::BubbleAnchor GetBubbleAnchor(actions::ActionId id) override;
  void GetBubbleAnchorAsync(
      actions::ActionId id,
      base::OnceCallback<void(BubbleAnchorResult)> callback) override;
  PinnedActionToolbarButton* GetChromeLabsButton() override;
  void UpdatePinnedStateAndAnnounce(actions::ActionId id, bool pin) override;
  void MovePinnedAction(actions::ActionId id, int index) override;
  void MovePinnedActionBy(actions::ActionId id, int delta) override;
  void OnActionsChanged() override;

 private:
  class PageAction;
  struct Menu;
  struct PendingAnchor {
    actions::ActionId id;
    base::OnceCallback<void(BubbleAnchorResult)> callback;
  };
  void RequestNativeFocus(uint32_t element);
  void NotifyMenuClosed();
  void InvalidateMenu(uint32_t request);
  void RepublishMenu();
  void OnFocus() override;

  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<CommandUpdater> commands_;
  const raw_ref<ui::AcceleratorProvider> accelerators_;
  const raw_ref<WindowSession> session_;
  const raw_ref<ControlAnchors> anchors_;
  const raw_ref<PinnedToolbarActionsModel> pinned_model_;
  raw_ptr<ExtensionsContainerViews> extensions_container_ = nullptr;
  const base::RepeatingClosure changed_;
  const MenuModelSnapshot::PublishImage images_;
  const base::RepeatingCallback<void(KrImageId)> release_image_;
  ui::ScopedUnownedUserData<ToolbarButtonProvider> registration_;
  base::ScopedObservation<PinnedToolbarActionsModel,
                          PinnedToolbarActionsModel::Observer>
      observation_{this};
  base::ObserverList<AppMenuButtonObserver>::Unchecked menu_observers_;
  std::unique_ptr<Menu> menu_;
  base::flat_map<actions::ActionId, std::unique_ptr<PageAction>> page_actions_;
  std::vector<actions::ActionId> popped_out_;
  std::vector<actions::ActionId> active_;
  std::vector<PendingAnchor> pending_anchors_;
  uint32_t menu_request_ = 0;
  bool menu_open_ = false;
  bool dev_tools_ = false;
  Mode mode_ = Mode::kReload;
  AppMenuIconController::TypeAndSeverity app_menu_state_{};
  base::WeakPtrFactory<NativeToolbar> weak_factory_{this};
};
}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_TOOLBAR_H_
