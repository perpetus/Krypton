// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_MENU_MODEL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SIDEBAR_MENU_MODEL_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "krypton_ui/abi.h"
#include "ui/actions/actions.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
// The empty tab-list menu borrows Chromium actions. Only workspace creation
// and saved Krypton layout/settings commands belong to this model.
class SidebarMenuModel final : public ui::SimpleMenuModel,
                               public ui::SimpleMenuModel::Delegate {
 public:
  enum Command : int {
    kCompact = 0x6d000000,
    kNewWorkspace,
    kSidebarRight,
    kAppearance,
    kEditTheme,
  };
  struct Action {
    int command;
    base::WeakPtr<actions::ActionItem> source;
    std::u16string fallback_label;
  };
  struct State {
    uint32_t layout_flags = 0;
    bool can_create_workspace = false;
    bool can_show_settings = false;
    bool can_edit_theme = false;
  };
  using ReadState = base::RepeatingCallback<std::optional<State>()>;
  using CanExecute = base::RepeatingCallback<bool(int)>;
  using Dispatch = base::RepeatingCallback<void(Command)>;

  SidebarMenuModel(base::span<const Action> actions,
                   ReadState read_state,
                   CanExecute can_execute,
                   Dispatch dispatch);
  ~SidebarMenuModel() override;

  bool IsCommandIdChecked(int command) const override;
  bool IsCommandIdEnabled(int command) const override;
  bool IsCommandIdVisible(int command) const override;
  bool GetAcceleratorForCommandId(int command,
                                  ui::Accelerator* accelerator) const override;
  void ExecuteCommand(int command, int event_flags) override;
  bool IsItemDynamicAt(size_t index) const override;
  std::u16string GetLabelAt(size_t index) const override;
  ui::ImageModel GetIconAt(size_t index) const override;

  [[nodiscard]] static std::string_view Glyph(int command);

 private:
  [[nodiscard]] actions::ActionItem* FindAction(int command) const;
  const std::vector<Action> actions_;
  const ReadState read_state_;
  const CanExecute can_execute_;
  const Dispatch dispatch_;
};
}  // namespace krypton_ui
#endif
