// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_MENUS_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_MENUS_H_

#include <memory>
#include <optional>
#include <string>

#include "base/containers/flat_map.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "chrome/browser/ui/krypton_ui/menu_model_snapshot.h"
#include "components/renderer_context_menu/render_view_context_menu_base.h"
#include "ui/actions/action_id.h"

class BrowserWindowInterface;
namespace tabs {
class TabInterface;
}
namespace gfx {
class Point;
}

namespace krypton_ui {
class WindowSession;
class BrowserAdapter;

// Adapts Chromium menu models; the module remains their only presentation.
// Source models and image leases survive dismissal through deferred execution.
class NativeMenus {
 public:
  using ExternalMenuFactory =
      base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(uint32_t,
                                                             uint64_t)>;
  void SetExternalMenuFactory(ExternalMenuFactory factory);
  using ToggleEssential = base::RepeatingCallback<void(tabs::TabInterface&)>;
  NativeMenus(
      BrowserWindowInterface& browser,
      WindowSession& session,
      MenuModelSnapshot::PublishImage images,
      base::RepeatingCallback<void(KrImageId)> release_image,
      base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(KrSpaceId)>
          workspace_menu,
      base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(KrActionId)>
          extension_menu,
      base::RepeatingCallback<
          std::unique_ptr<ui::MenuModel>(uint32_t, tabs::TabInterface*)>
          context_menu,
      ToggleEssential toggle_essential);
  ~NativeMenus();
  NativeMenus(const NativeMenus&) = delete;
  NativeMenus& operator=(const NativeMenus&) = delete;

  // `target` is the request's target id; `tab` is the tab it names, or the
  // active tab for menus that act on it.
  [[nodiscard]] base::expected<void, std::string> Present(
      uint32_t request,
      uint32_t kind,
      uint64_t target,
      tabs::TabInterface* tab,
      const ui::ColorProvider* colors);
  [[nodiscard]] base::OnceClosure CaptureCommand(uint32_t request,
                                                 int32_t command);
  void Closed(uint32_t request);
  void CloseAll();
  [[nodiscard]] bool HasOpenMenu() const {
    return !sources_.empty() || !page_menus_.empty();
  }
  [[nodiscard]] static std::optional<int> SubmenuCommand(
      actions::ActionId action);
  [[nodiscard]] base::ScopedClosureRunner RequestPageMenu(ui::MenuModel& model,
                                                          KrRect anchor);
  // Republishes a presented page menu after rows change in place. A pending
  // request needs nothing: it captures the current rows when presented.
  void RefreshPageMenu(const ui::MenuModel& model,
                       const ui::ColorProvider* colors);

 private:
  struct Source;
  [[nodiscard]] base::expected<void, std::string>
  Publish(uint32_t request, Source& source, const ui::ColorProvider* colors);
  void Republish(uint32_t request);
  void Dismiss(uint32_t request);
  void Invalidated(uint32_t request);
  void CancelPageMenu(uint64_t target);
  const base::WeakPtr<BrowserWindowInterface> browser_;
  const raw_ref<WindowSession> session_;
  const MenuModelSnapshot::PublishImage images_;
  const base::RepeatingCallback<void(KrImageId)> release_image_;
  const base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(KrSpaceId)>
      workspace_menu_;
  const base::RepeatingCallback<std::unique_ptr<ui::MenuModel>(KrActionId)>
      extension_menu_;
  const base::RepeatingCallback<
      std::unique_ptr<ui::MenuModel>(uint32_t, tabs::TabInterface*)>
      context_menu_;
  const ToggleEssential toggle_essential_;
  ExternalMenuFactory external_menu_;
  base::flat_map<uint32_t, std::unique_ptr<Source>> sources_;
  base::flat_map<uint64_t, base::WeakPtr<ui::MenuModel>> page_menus_;
  uint64_t next_page_menu_ = 0;
  base::WeakPtrFactory<NativeMenus> weak_factory_{this};
};

// The renderer keeps its Chromium context-menu model and command delegate.
// This toolkit delegate gives their presentation to KryptonUI.
class NativeContextMenu final
    : public RenderViewContextMenuBase::ToolkitDelegate {
 public:
  explicit NativeContextMenu(base::WeakPtr<BrowserAdapter> owner);
  ~NativeContextMenu() override;
  void Show(const gfx::Point& screen_point);
  void Init(ui::SimpleMenuModel* model) override;
  void Cancel() override;
  void UpdateMenuItem(int command_id,
                      bool enabled,
                      bool hidden,
                      const std::u16string& title) override;
  void RebuildMenu() override;

 private:
  void Refresh();
  void Republish();
  base::WeakPtr<BrowserAdapter> owner_;
  base::WeakPtr<ui::MenuModel> model_;
  base::ScopedClosureRunner dismissal_;
  bool refresh_pending_ = false;
  base::WeakPtrFactory<NativeContextMenu> weak_factory_{this};
};
}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_NATIVE_MENUS_H_
