// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_MENU_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_WORKSPACE_MENU_H_

#include <memory>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "chrome/browser/ui/krypton_ui/workspace_prefs.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
// A tab context menu captures its own tab in the callbacks, independently of
// the selected tab and the workspace switcher's move-active-tab operation.
class WorkspaceTabMenuModel final : public ui::SimpleMenuModel,
                                    public ui::SimpleMenuModel::Delegate {
 public:
  static constexpr int kSubmenu = 0x6e000000;
  static constexpr int kEssential = 0x6f000000;
  using CanMove = base::RepeatingCallback<bool(KrSpaceId)>;
  using Move = base::RepeatingCallback<void(KrSpaceId)>;
  WorkspaceTabMenuModel(const WorkspaceCatalog& catalog,
                        KrSpaceId current,
                        CanMove can_move,
                        Move move);
  ~WorkspaceTabMenuModel() override;
  [[nodiscard]] bool HasCommand(int command) const;
  bool IsCommandIdEnabled(int command) const override;
  void ExecuteCommand(int command, int event_flags) override;

 private:
  struct Entry {
    int command;
    KrSpaceId space;
  };
  const CanMove can_move_;
  const Move move_;
  std::vector<Entry> entries_;
};

// Krypton's workspace operations, backed by the Chromium profile catalog.
// Browser commands remain in Chromium's app menu.
class WorkspaceMenuModel final : public ui::SimpleMenuModel,
                                 public ui::SimpleMenuModel::Delegate {
 public:
  enum class Operation { kSwitch, kCreate, kEdit, kMoveTab, kTheme };
  using Dispatch = base::RepeatingCallback<void(Operation, KrSpaceId)>;
  WorkspaceMenuModel(const WorkspaceCatalog& catalog,
                     KrSpaceId active,
                     KrSpaceId target,
                     bool can_move_tab,
                     Dispatch dispatch,
                     bool can_edit_theme = true);
  ~WorkspaceMenuModel() override;
  bool IsCommandIdChecked(int command) const override;
  bool IsCommandIdEnabled(int command) const override;
  void ExecuteCommand(int command, int event_flags) override;
  [[nodiscard]] static std::string_view Glyph(int command);
  [[nodiscard]] static constexpr bool Handles(uint32_t kind) {
    return kind == KR_MENU_SPACE || kind == KR_MENU_WORKSPACES;
  }

 private:
  struct Entry {
    int command;
    Operation operation;
    KrSpaceId space;
  };
  const KrSpaceId active_;
  const Dispatch dispatch_;
  const bool can_edit_theme_;
  std::vector<Entry> entries_;
  std::unique_ptr<ui::SimpleMenuModel> move_menu_;
};
}  // namespace krypton_ui
#endif
