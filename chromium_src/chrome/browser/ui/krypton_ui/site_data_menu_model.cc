// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/site_data_menu_model.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/strings/string_util.h"
#include "ui/base/accelerators/accelerator.h"

namespace krypton_ui {
SiteDataMenuModel::SiteDataMenuModel(
    base::span<const Action> actions,
    std::unique_ptr<ui::MenuModel> extensions,
    std::unique_ptr<ui::MenuModel> more,
    Labels labels,
    ReadState state,
    base::RepeatingCallback<bool(int)> can_execute,
    Dispatch dispatch,
    std::unique_ptr<ui::MenuModel> boosts)
    : ui::SimpleMenuModel(this),
      actions_(actions.begin(), actions.end()),
      labels_(std::move(labels)),
      state_(std::move(state)),
      can_execute_(std::move(can_execute)),
      dispatch_(std::move(dispatch)),
      extensions_(std::move(extensions)),
      more_(std::move(more)) {
  for (const auto& action : actions_) {
    page_actions_.AddCheckItem(action.command, action.fallback_label);
  }
  if (!labels_.screenshot.empty()) {
    page_actions_.InsertItemAt(
        std::min<size_t>(1, page_actions_.GetItemCount()), kScreenshot,
        labels_.screenshot);
  }
  if (!labels_.boosts.empty()) {
    page_actions_.InsertItemAt(
        std::min<size_t>(2, page_actions_.GetItemCount()), kBoosts,
        labels_.boosts);
  }
  AddSubMenu(kPageActions, {}, &page_actions_);
  AddSubMenu(kExtensionsHeader, labels_.extensions, &empty_);
  AddSubMenu(kExtensionsGrid, labels_.extensions,
             extensions_ ? extensions_.get() : &empty_);
  layout_ = {{kPageActions, KR_MENU_ITEM_GRID},
             {kExtensionsHeader, KR_MENU_ITEM_SECTION},
             {kExtensionsGrid, KR_MENU_ITEM_GRID}};
  boosts_ = std::move(boosts);
  if (boosts_ && boosts_->GetItemCount()) {
    AddSubMenu(kBoostsSection, u"Boosts", &empty_);
    layout_.push_back({kBoostsSection, KR_MENU_ITEM_SECTION});
    for (size_t index = 0; index < boosts_->GetItemCount(); ++index) {
      const int command = kBoostRowFirst + static_cast<int>(index);
      AddSubMenu(command, boosts_->GetLabelAt(index),
                 boosts_->GetSubmenuModelAt(index));
      layout_.push_back({command, KR_MENU_ITEM_CONTROLS});
    }
  }
  layout_.push_back({kSettingsHeader, KR_MENU_ITEM_SECTION});
  layout_.push_back({kFooter, KR_MENU_ITEM_FOOTER});
  AddSubMenu(kSettingsHeader, labels_.settings, &empty_);
  AddItem(kProtection, labels_.protection);
  const auto current = state_.Run();
  footer_.AddItem(current && current->secure ? kSecureConnection : kSecurity,
                  {});
  footer_.AddItem(kSiteSettings, labels_.site_settings);
  if (more_) {
    footer_.AddSubMenu(kMore, labels_.more, more_.get());
  }
  AddSubMenu(kFooter, labels_.settings, &footer_);
}
SiteDataMenuModel::~SiteDataMenuModel() = default;

const SiteDataMenuModel::Action* SiteDataMenuModel::Find(int command) const {
  const auto found = std::ranges::find(actions_, command, &Action::command);
  return found == actions_.end() ? nullptr : &*found;
}
bool SiteDataMenuModel::IsCommandIdChecked(int command) const {
  const auto* action = Find(command);
  return action && action->source && action->source->GetChecked();
}
bool SiteDataMenuModel::IsCommandIdEnabled(int command) const {
  const auto state = state_.Run();
  if (!state) {
    return false;
  }
  if (command == kBoostsSection ||
      (command >= kBoostRowFirst && command <= kBoostRowLast)) {
    return state->boost_available;
  }
  if (command == kScreenshot || command == kBoosts) {
    return state->site_available && can_execute_.Run(command);
  }
  if (const auto* action = Find(command)) {
    return action->source && action->source->GetEnabled() &&
           action->source->GetVisible() && can_execute_.Run(command);
  }
  if (command == kSecurity || command == kSecureConnection ||
      command == kProtection || command == kSiteSettings) {
    return state->site_available;
  }
  return command >= kPageActions && command <= kMore;
}
bool SiteDataMenuModel::IsCommandIdVisible(int command) const {
  if (const auto* action = Find(command)) {
    return action->source && action->source->GetVisible();
  }
  if (command == kProtection) {
    const auto state = state_.Run();
    return state && state->site_available;
  }
  return true;
}
bool SiteDataMenuModel::IsItemForCommandIdDynamic(int command) const {
  return Find(command) || command == kSecurity || command == kSecureConnection;
}
std::u16string SiteDataMenuModel::GetLabelForCommandId(int command) const {
  if (const auto* action = Find(command)) {
    if (!action->source || action->source->GetText().empty()) {
      return action->fallback_label;
    }
    std::u16string label(action->source->GetText());
    base::ReplaceSubstringsAfterOffset(&label, 0, u"&", u"&&");
    return label;
  }
  if (command == kSecurity || command == kSecureConnection) {
    const auto state = state_.Run();
    return state ? state->security_label : std::u16string();
  }
  return {};
}
bool SiteDataMenuModel::GetAcceleratorForCommandId(
    int command,
    ui::Accelerator* accelerator) const {
  const auto* action = Find(command);
  if (!action || !action->source ||
      action->source->GetAccelerator().IsEmpty()) {
    return false;
  }
  *accelerator = action->source->GetAccelerator();
  return true;
}
void SiteDataMenuModel::ExecuteCommand(int command, int event_flags) {
  if (!IsCommandIdEnabled(command)) {
    return;
  }
  if (const auto* action = Find(command)) {
    action->source->InvokeAction();
  } else if (command == kSecurity || command == kSecureConnection ||
             command == kSiteSettings || command == kProtection ||
             command == kScreenshot || command == kBoosts) {
    dispatch_.Run(static_cast<Command>(command));
  }
}
std::u16string SiteDataMenuModel::GetSecondaryLabelAt(size_t index) const {
  const int command = GetCommandIdAt(index);
  if (boosts_ && command >= kBoostRowFirst && command <= kBoostRowLast) {
    const auto boost = static_cast<size_t>(command - kBoostRowFirst);
    if (boost < boosts_->GetItemCount()) {
      return boosts_->GetSecondaryLabelAt(boost);
    }
  }
  if (GetCommandIdAt(index) == kProtection) {
    const auto state = state_.Run();
    return state ? state->protection_status : std::u16string();
  }
  return ui::SimpleMenuModel::GetSecondaryLabelAt(index);
}
base::span<const MenuPanelRow> SiteDataMenuModel::Layout() {
  static constexpr std::array<MenuPanelRow, 5> rows{{
      {kPageActions, KR_MENU_ITEM_GRID},
      {kExtensionsHeader, KR_MENU_ITEM_SECTION},
      {kExtensionsGrid, KR_MENU_ITEM_GRID},
      {kSettingsHeader, KR_MENU_ITEM_SECTION},
      {kFooter, KR_MENU_ITEM_FOOTER},
  }};
  return rows;
}
std::string_view SiteDataMenuModel::Glyph(int command) {
  if (command >= kBoostRowFirst && command <= kBoostRowLast) {
    return "\uE771";
  }
  switch (command) {
    case kBoosts:
      return "\uE771";
    case kScreenshot:
      return "\uE7C5";
    case kSecureConnection:
      return "\uE72E";
    case kSecurity:
      return "\uE946";
    case kProtection:
      return "\uEA18";
    case kSiteSettings:
      return "\uE713";
    case kMore:
      return "\uE712";
    case kManageExtensions:
      return "\uE710";
    default:
      return {};
  }
}
}  // namespace krypton_ui
