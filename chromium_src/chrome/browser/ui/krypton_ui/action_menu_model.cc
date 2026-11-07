// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/action_menu_model.h"

#include <limits>
#include <utility>

namespace krypton_ui {
ActionMenuModel::ActionMenuModel(ui::MenuModel& source,
                                 std::optional<Action> action)
    : source_(source.AsWeakPtr()), action_(std::move(action)) {}
ActionMenuModel::ActionMenuModel(std::unique_ptr<ui::MenuModel> source,
                                 std::vector<Submenu> submenus)
    : owned_source_(std::move(source)),
      submenus_(std::move(submenus)),
      source_(owned_source_->AsWeakPtr()) {}
ActionMenuModel::~ActionMenuModel() = default;
base::WeakPtr<ui::MenuModel> ActionMenuModel::AsWeakPtr() {
  return weak_factory_.GetWeakPtr();
}
bool ActionMenuModel::IsSource(size_t index) const {
  return source_ && index >= Offset() &&
         index - Offset() < source_->GetItemCount();
}
size_t ActionMenuModel::GetItemCount() const {
  return source_ ? source_->GetItemCount() + Offset() : 0;
}
ui::MenuModel::ItemType ActionMenuModel::GetTypeAt(size_t index) const {
  if (IsSource(index))
    return source_->GetTypeAt(index - Offset());
  if (index < submenus_.size())
    return TYPE_SUBMENU;
  return action_ && index == 0 ? TYPE_COMMAND : TYPE_SEPARATOR;
}
ui::MenuSeparatorType ActionMenuModel::GetSeparatorTypeAt(size_t index) const {
  return IsSource(index) ? source_->GetSeparatorTypeAt(index - Offset())
                         : ui::NORMAL_SEPARATOR;
}
int ActionMenuModel::GetCommandIdAt(size_t index) const {
  if (IsSource(index))
    return source_->GetCommandIdAt(index - Offset());
  if (index < submenus_.size())
    return submenus_[index].command;
  return action_ && index == 0 ? std::numeric_limits<int>::max() : -1;
}
std::u16string ActionMenuModel::GetLabelAt(size_t index) const {
  if (index < submenus_.size())
    return submenus_[index].label;
  return IsSource(index)
             ? source_->GetLabelAt(index - Offset())
             : (action_ && index == 0 ? action_->label : std::u16string());
}
std::u16string ActionMenuModel::GetSecondaryLabelAt(size_t index) const {
  return IsSource(index) ? source_->GetSecondaryLabelAt(index - Offset())
                         : std::u16string();
}
std::u16string ActionMenuModel::GetMinorTextAt(size_t index) const {
  return IsSource(index) ? source_->GetMinorTextAt(index - Offset())
                         : std::u16string();
}
bool ActionMenuModel::GetMinorTextIsUrlAt(size_t index) const {
  return IsSource(index) && source_->GetMinorTextIsUrlAt(index - Offset());
}
ui::ImageModel ActionMenuModel::GetMinorIconAt(size_t index) const {
  return IsSource(index) ? source_->GetMinorIconAt(index - Offset())
                         : ui::ImageModel();
}
bool ActionMenuModel::IsItemDynamicAt(size_t index) const {
  return IsSource(index) && source_->IsItemDynamicAt(index - Offset());
}
bool ActionMenuModel::MayHaveMnemonicsAt(size_t index) const {
  return IsSource(index) && source_->MayHaveMnemonicsAt(index - Offset());
}
std::u16string ActionMenuModel::GetAccessibleNameAt(size_t index) const {
  return IsSource(index) ? source_->GetAccessibleNameAt(index - Offset())
                         : GetLabelAt(index);
}
const gfx::FontList* ActionMenuModel::GetLabelFontListAt(size_t index) const {
  return IsSource(index) ? source_->GetLabelFontListAt(index - Offset())
                         : nullptr;
}
bool ActionMenuModel::GetAcceleratorAt(size_t index,
                                       ui::Accelerator* accelerator) const {
  return IsSource(index) &&
         source_->GetAcceleratorAt(index - Offset(), accelerator);
}
bool ActionMenuModel::GetForceShowAcceleratorForItemAt(size_t index) const {
  return IsSource(index) &&
         source_->GetForceShowAcceleratorForItemAt(index - Offset());
}
bool ActionMenuModel::IsItemCheckedAt(size_t index) const {
  return IsSource(index) && source_->IsItemCheckedAt(index - Offset());
}
int ActionMenuModel::GetGroupIdAt(size_t index) const {
  return IsSource(index) ? source_->GetGroupIdAt(index - Offset()) : -1;
}
ui::ImageModel ActionMenuModel::GetIconAt(size_t index) const {
  return IsSource(index)
             ? source_->GetIconAt(index - Offset())
             : (action_ && index == 0 ? action_->icon : ui::ImageModel());
}
ui::ButtonMenuItemModel* ActionMenuModel::GetButtonMenuItemAt(
    size_t index) const {
  return IsSource(index) ? source_->GetButtonMenuItemAt(index - Offset())
                         : nullptr;
}
bool ActionMenuModel::IsEnabledAt(size_t index) const {
  if (IsSource(index))
    return source_->IsEnabledAt(index - Offset());
  if (index < submenus_.size())
    return source_ && submenus_[index].model &&
           submenus_[index].model->GetItemCount();
  return source_ && action_ && index == 0 && action_->enabled.Run();
}
bool ActionMenuModel::IsVisibleAt(size_t index) const {
  return IsSource(index)
             ? source_->IsVisibleAt(index - Offset())
             : source_ && (action_ || !submenus_.empty()) && index < Offset();
}
bool ActionMenuModel::IsAlertedAt(size_t index) const {
  return IsSource(index) && source_->IsAlertedAt(index - Offset());
}
bool ActionMenuModel::IsNewFeatureAt(size_t index) const {
  return IsSource(index) && source_->IsNewFeatureAt(index - Offset());
}
std::optional<ui::NewBadgeType> ActionMenuModel::GetNewBadgeTypeAt(
    size_t index) const {
  return IsSource(index) ? source_->GetNewBadgeTypeAt(index - Offset())
                         : std::nullopt;
}
ui::ElementIdentifier ActionMenuModel::GetElementIdentifierAt(
    size_t index) const {
  return IsSource(index) ? source_->GetElementIdentifierAt(index - Offset())
                         : ui::ElementIdentifier();
}
ui::MenuModel* ActionMenuModel::GetSubmenuModelAt(size_t index) const {
  if (index < submenus_.size())
    return submenus_[index].model.get();
  return IsSource(index) ? source_->GetSubmenuModelAt(index - Offset())
                         : nullptr;
}
void ActionMenuModel::ActivatedAt(size_t index) {
  ActivatedAt(index, 0);
}
void ActionMenuModel::ActivatedAt(size_t index, int event_flags) {
  if (!IsEnabledAt(index) || !IsVisibleAt(index))
    return;
  if (IsSource(index))
    source_->ActivatedAt(index - Offset(), event_flags);
  else if (action_ && index == 0)
    action_->invoke.Run();
}
void ActionMenuModel::MenuWillShow() {
  if (source_)
    source_->MenuWillShow();
}
void ActionMenuModel::MenuWillClose() {
  if (source_)
    source_->MenuWillClose();
}
std::optional<ui::ColorId> ActionMenuModel::GetForegroundColorId(size_t index) {
  return IsSource(index) ? source_->GetForegroundColorId(index - Offset())
                         : std::nullopt;
}
std::optional<ui::ColorId> ActionMenuModel::GetSubmenuBackgroundColorId(
    size_t index) {
  return IsSource(index)
             ? source_->GetSubmenuBackgroundColorId(index - Offset())
             : std::nullopt;
}
std::optional<ui::ColorId> ActionMenuModel::GetSelectedBackgroundColorId(
    size_t index) {
  return IsSource(index)
             ? source_->GetSelectedBackgroundColorId(index - Offset())
             : std::nullopt;
}
}  // namespace krypton_ui
