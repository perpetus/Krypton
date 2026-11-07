// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_EXTENSIONS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_EXTENSIONS_H_

#include <memory>
#include <optional>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/extensions/extensions_toolbar_view_model.h"
#include "chrome/browser/ui/krypton_ui/extension_actions.h"
#include "chrome/browser/ui/krypton_ui/menu_model_snapshot.h"
#include "chrome/browser/ui/views/extensions/extensions_container_views.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

namespace krypton_ui {
class ControlAnchors;
class WindowSession;
class NativeExtensions final : public ExtensionsToolbarViewModel::Delegate,
                               public ExtensionsToolbarViewModel::Observer,
                               public ExtensionsContainerViews {
 public:
  static constexpr KrActionId kMenuAction = kExtensionMenuAction;
  static constexpr bool Handles(KrActionId id) { return IsExtensionAction(id); }
  NativeExtensions(BrowserWindowInterface& browser,
                   views::View& host,
                   WindowSession& session,
                   ControlAnchors& anchors,
                   MenuModelSnapshot::PublishImage images,
                   base::RepeatingCallback<void(KrImageId)> release,
                   base::RepeatingClosure changed,
                   base::RepeatingClosure close_menus);
  ~NativeExtensions() override;
  void Attach();
  void Close();
  void AppendActions(std::vector<KrAction>& actions);
  void RefreshImages();
  void Invoke(KrActionId id);
  std::unique_ptr<ui::MenuModel> CreateMenu(KrActionId id, bool panel = false);

  // Chromium retains extension permissions, pin preferences, command handling,
  // popup hosts and real context menus. Only their chrome is presented
  // natively.
  std::unique_ptr<ExtensionActionViewModel> CreateActionViewModel(
      const ToolbarActionsModel::ActionId& id,
      ExtensionsContainer* container) override;
  void HideActivePopup() override;
  void CloseExtensionsMenuIfOpen() override;
  bool CanShowToolbarActionPopupForAPICall(
      const ToolbarActionsModel::ActionId& id) override;
  void ToggleExtensionsMenu() override;
  void OnActionsInitialized() override;
  void OnActionAdded(const ToolbarActionsModel::ActionId& id) override;
  void OnActionRemoved(const ToolbarActionsModel::ActionId& id) override;
  void OnActionUpdated(const ToolbarActionsModel::ActionId& id) override;
  void OnPinnedActionsChanged() override;
  void OnActiveWebContentsChanged(bool same_document,
                                  content::WebContents* contents) override;
  void OnToolbarControlStateUpdated() override;
  bool IsVisible() const override;
  std::optional<extensions::ExtensionId> GetPoppedOutActionId() const override;
  bool IsActionVisibleOnToolbar(const std::string& id) const override;
  void UndoPopOut() override;
  void SetPopupOwner(ToolbarActionViewModel* owner) override;
  void PopOutAction(const extensions::ExtensionId& id,
                    base::OnceClosure ready) override;
  void ShowWidgetForExtension(views::Widget* widget,
                              const std::string& id) override;
  void CollapseConfirmation() override {}
  void ShowContextMenuAsFallback(const extensions::ExtensionId& id) override;
  void OnPopupShown(const extensions::ExtensionId& id, bool by_user) override;
  void OnPopupClosed(const extensions::ExtensionId& id) override;
  views::FocusManager* GetFocusManagerForAccelerator() override;
  views::BubbleAnchor GetReferenceButtonForPopup(
      const extensions::ExtensionId& id) override;
  views::BubbleAnchor GetExtensionsButtonAnchor() override;
  views::BubbleBorder::Arrow GetPopupArrow() const override;

 private:
  struct Action;
  void EnsureAction(const std::string& id);
  void AppendAction(const std::string& id, std::vector<KrAction>& actions);
  void UpdateBadge(Action& action, content::WebContents* contents);
  bool CanInvoke(KrActionId id) const;
  void InvokeFromMenu(KrActionId id);
  Action* Find(KrActionId id) const;
  void RequestMenu(KrActionId id);
  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<views::View> host_;
  const raw_ref<WindowSession> session_;
  const raw_ref<ControlAnchors> anchors_;
  const MenuModelSnapshot::PublishImage images_;
  const base::RepeatingCallback<void(KrImageId)> release_;
  const base::RepeatingClosure changed_;
  const base::RepeatingClosure close_menus_;
  std::unique_ptr<ExtensionsToolbarViewModel> model_;
  std::optional<ui::ScopedUnownedUserData<ExtensionsContainer>> registration_;
  base::flat_map<std::string, std::unique_ptr<Action>> actions_;
  std::optional<std::string> popped_;
  raw_ptr<ToolbarActionViewModel> popup_owner_ = nullptr;
  std::string menu_label_;
  uint64_t next_id_ = 0;
  bool attached_ = false;
  bool closed_ = false;
  base::WeakPtrFactory<NativeExtensions> weak_factory_{this};
};
}  // namespace krypton_ui
#endif
