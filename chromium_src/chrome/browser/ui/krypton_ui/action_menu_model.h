// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_ACTION_MENU_MODEL_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_ACTION_MENU_MODEL_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "ui/base/models/image_model.h"
#include "ui/base/models/menu_model.h"

namespace krypton_ui {
// Borrows Chromium's context model without copying its command indices or
// metadata. An optional leading action exposes the actual extension action
// alongside its original pin/options/site-access commands.
class ActionMenuModel final : public ui::MenuModel {
 public:
  struct Submenu {
    int command;
    std::u16string label;
    std::unique_ptr<ui::MenuModel> model;
  };
  ActionMenuModel(std::unique_ptr<ui::MenuModel> source,
                  std::vector<Submenu> submenus);
  struct Action {
    std::u16string label;
    ui::ImageModel icon;
    base::RepeatingCallback<bool()> enabled;
    base::RepeatingClosure invoke;
  };
  explicit ActionMenuModel(ui::MenuModel& source,
                           std::optional<Action> action = std::nullopt);
  ~ActionMenuModel() override;
  base::WeakPtr<ui::MenuModel> AsWeakPtr() override;
  size_t GetItemCount() const override;
  ItemType GetTypeAt(size_t index) const override;
  ui::MenuSeparatorType GetSeparatorTypeAt(size_t index) const override;
  int GetCommandIdAt(size_t index) const override;
  std::u16string GetLabelAt(size_t index) const override;
  std::u16string GetSecondaryLabelAt(size_t index) const override;
  std::u16string GetMinorTextAt(size_t index) const override;
  bool GetMinorTextIsUrlAt(size_t index) const override;
  ui::ImageModel GetMinorIconAt(size_t index) const override;
  bool IsItemDynamicAt(size_t index) const override;
  bool MayHaveMnemonicsAt(size_t index) const override;
  std::u16string GetAccessibleNameAt(size_t index) const override;
  const gfx::FontList* GetLabelFontListAt(size_t index) const override;
  bool GetAcceleratorAt(size_t index,
                        ui::Accelerator* accelerator) const override;
  bool GetForceShowAcceleratorForItemAt(size_t index) const override;
  bool IsItemCheckedAt(size_t index) const override;
  int GetGroupIdAt(size_t index) const override;
  ui::ImageModel GetIconAt(size_t index) const override;
  ui::ButtonMenuItemModel* GetButtonMenuItemAt(size_t index) const override;
  bool IsEnabledAt(size_t index) const override;
  bool IsVisibleAt(size_t index) const override;
  bool IsAlertedAt(size_t index) const override;
  bool IsNewFeatureAt(size_t index) const override;
  std::optional<ui::NewBadgeType> GetNewBadgeTypeAt(
      size_t index) const override;
  ui::ElementIdentifier GetElementIdentifierAt(size_t index) const override;
  ui::MenuModel* GetSubmenuModelAt(size_t index) const override;
  void ActivatedAt(size_t index) override;
  void ActivatedAt(size_t index, int event_flags) override;
  void MenuWillShow() override;
  void MenuWillClose() override;
  std::optional<ui::ColorId> GetForegroundColorId(size_t index) override;
  std::optional<ui::ColorId> GetSubmenuBackgroundColorId(size_t index) override;
  std::optional<ui::ColorId> GetSelectedBackgroundColorId(
      size_t index) override;

 private:
  bool IsSource(size_t index) const;
  size_t Offset() const {
    return submenus_.empty() ? (action_ ? 2 : 0) : submenus_.size() + 1;
  }
  std::unique_ptr<ui::MenuModel> owned_source_;
  std::vector<Submenu> submenus_;
  const base::WeakPtr<ui::MenuModel> source_;
  const std::optional<Action> action_;
  base::WeakPtrFactory<ActionMenuModel> weak_factory_{this};
};
}  // namespace krypton_ui
#endif
