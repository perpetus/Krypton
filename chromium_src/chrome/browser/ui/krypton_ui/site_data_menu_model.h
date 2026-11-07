// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_SITE_DATA_MENU_MODEL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_SITE_DATA_MENU_MODEL_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/ui/krypton_ui/menu_panel_layout.h"
#include "chrome/browser/ui/krypton_ui/sidebar_menu_model.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
inline constexpr KrActionId kSiteToolsAction = uint64_t{5} << 56;

// A native presentation of browser-owned actions and extension menus. The
// caller supplies localized Chromium labels and live, tab-scoped state.
class SiteDataMenuModel final : public ui::SimpleMenuModel,
                                public ui::SimpleMenuModel::Delegate {
 public:
  enum Command {
    kPageActions = 0x6c000000,
    kExtensionsHeader,
    kExtensionsGrid,
    kSettingsHeader,
    kProtection,
    kFooter,
    kSecurity,
    kSecureConnection,
    kSiteSettings,
    kMore,
    kManageExtensions,
    kScreenshot,
    kBoosts,
    kBoostsSection,
    kBoostRowFirst = 0x6c100000,
    kBoostRowLast = kBoostRowFirst + 0xffff,
  };
  struct Labels {
    std::u16string extensions, settings, protection, site_settings, more,
        screenshot, boosts;
  };
  struct State {
    bool site_available = false;
    bool secure = false;
    std::u16string security_label, protection_status;
    bool boost_available = false;
  };
  using Action = SidebarMenuModel::Action;
  using ReadState = base::RepeatingCallback<std::optional<State>()>;
  using Dispatch = base::RepeatingCallback<void(Command)>;

  SiteDataMenuModel(base::span<const Action> actions,
                    std::unique_ptr<ui::MenuModel> extensions,
                    std::unique_ptr<ui::MenuModel> more,
                    Labels labels,
                    ReadState state,
                    base::RepeatingCallback<bool(int)> can_execute,
                    Dispatch dispatch,
                    std::unique_ptr<ui::MenuModel> boosts = nullptr);
  ~SiteDataMenuModel() override;
  bool IsCommandIdChecked(int command) const override;
  bool IsCommandIdEnabled(int command) const override;
  bool IsCommandIdVisible(int command) const override;
  bool IsItemForCommandIdDynamic(int command) const override;
  std::u16string GetLabelForCommandId(int command) const override;
  bool GetAcceleratorForCommandId(int command,
                                  ui::Accelerator* accelerator) const override;
  void ExecuteCommand(int command, int event_flags) override;
  std::u16string GetSecondaryLabelAt(size_t index) const override;

  static base::span<const MenuPanelRow> Layout();
  base::span<const MenuPanelRow> PanelLayout() const { return layout_; }
  static std::string_view Glyph(int command);

 private:
  const Action* Find(int command) const;
  const std::vector<Action> actions_;
  const Labels labels_;
  const ReadState state_;
  std::unique_ptr<ui::MenuModel> boosts_;
  std::vector<MenuPanelRow> layout_;
  const base::RepeatingCallback<bool(int)> can_execute_;
  const Dispatch dispatch_;
  ui::SimpleMenuModel page_actions_{this}, empty_{this}, footer_{this};
  std::unique_ptr<ui::MenuModel> extensions_, more_;
};
}  // namespace krypton_ui

#endif
