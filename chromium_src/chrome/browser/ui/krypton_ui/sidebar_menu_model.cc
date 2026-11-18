// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/sidebar_menu_model.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_util.h"
#include "chrome/app/chrome_command_ids.h"
#include "ui/base/accelerators/accelerator.h"

namespace krypton_ui {
namespace {
constexpr uint32_t kHorizontalLayouts =
    KR_LAYOUT_TABS_CLASSIC | KR_LAYOUT_TABS_COMPACT | KR_LAYOUT_TABS_DYNAMIC;
std::u16string Label(const actions::ActionItem& action) {
  std::u16string label(action.GetText());
  base::ReplaceSubstringsAfterOffset(&label, 0, u"&", u"&&");
  return label;
}
}  // namespace

SidebarMenuModel::SidebarMenuModel(base::span<const Action> actions,
                                   ReadState read_state,
                                   CanExecute can_execute,
                                   Dispatch dispatch,
                                   uint64_t zone)
    : ui::SimpleMenuModel(this),
      actions_(actions.begin(), actions.end()),
      read_state_(std::move(read_state)),
      can_execute_(std::move(can_execute)),
      dispatch_(std::move(dispatch)),
      layout_menus_(this),
      appearance_(this),
      page_(this) {
  for (const auto& action : actions_) {
    if (action.source) {
      auto& menu = action.command == IDC_RELOAD ||
                           action.command == IDC_BOOKMARK_THIS_TAB
                       ? page_
                       : *this;
      menu.AddItem(action.command, action.source->GetText().empty()
                                       ? action.fallback_label
                                       : Label(*action.source));
    }
  }
  if (zone == KR_SIDEBAR_TABS)
    AddItem(kNewFolder, u"New folder");
  if (zone == KR_SIDEBAR_ESSENTIALS)
    AddCheckItem(kToggleEssential, u"Use current tab as Essential");
  AddItem(kNewWorkspace, u"New workspace\u2026");
  if (page_.GetItemCount())
    AddSubMenu(kPageMenu, u"Current page", &page_);
  AddSeparator(ui::NORMAL_SEPARATOR);
  layout_menus_.AddTo(*this);
  appearance_.AddItem(kEditTheme, u"Edit theme\u2026");
  appearance_.AddItem(kAppearance, u"Appearance settings\u2026");
  AddSubMenu(kAppearanceMenu, u"Appearance", &appearance_);
}
SidebarMenuModel::~SidebarMenuModel() = default;

actions::ActionItem* SidebarMenuModel::FindAction(int command) const {
  const auto found = std::ranges::find(actions_, command, &Action::command);
  return found == actions_.end() ? nullptr : found->source.get();
}
bool SidebarMenuModel::IsCommandIdChecked(int command) const {
  const auto state = read_state_.Run();
  if (!state) {
    return false;
  }
  if (IsLayoutCommand(command))
    return IsLayoutCommandChecked(state->layout_flags, command);
  switch (command) {
    case kToggleEssential:
      return state->essential;
    case kCompact:
      return state->layout_flags & KR_LAYOUT_COMPACT_MODE;
    case kSidebarRight:
      return state->layout_flags & KR_LAYOUT_SIDEBAR_RIGHT;
    default:
      if (const auto* action = FindAction(command)) {
        return action->GetChecked();
      }
      return false;
  }
}
bool SidebarMenuModel::IsCommandIdEnabled(int command) const {
  const auto state = read_state_.Run();
  if (!state) {
    return false;
  }
  if (IsLayoutCommand(command))
    return IsLayoutCommandEnabled(state->layout_flags, command);
  switch (command) {
    case kNewFolder:
      return state->can_create_folder;
    case kToggleEssential:
      return state->can_set_essential;
    case kAppearanceMenu:
      return state->can_show_settings || state->can_edit_theme;
    case kPageMenu:
      return true;
    case kCompact:
      return true;
    case kSidebarRight:
      return !(state->layout_flags & kHorizontalLayouts);
    case kNewWorkspace:
      return state->can_create_workspace;
    case kAppearance:
      return state->can_show_settings;
    case kEditTheme:
      return state->can_edit_theme;
    default:
      if (const auto* action = FindAction(command)) {
        return action->GetEnabled() && action->GetVisible() &&
               can_execute_.Run(command);
      }
      return false;
  }
}
bool SidebarMenuModel::IsCommandIdVisible(int command) const {
  if (const auto* action = FindAction(command)) {
    return action->GetVisible();
  }
  return IsLayoutCommand(command) ||
         (command >= kCompact && command <= kPageMenu);
}
bool SidebarMenuModel::GetAcceleratorForCommandId(
    int command,
    ui::Accelerator* accelerator) const {
  const auto* action = FindAction(command);
  if (!action || action->GetAccelerator().IsEmpty()) {
    return false;
  }
  *accelerator = action->GetAccelerator();
  return true;
}
void SidebarMenuModel::ExecuteCommand(int command, int event_flags) {
  if (!IsCommandIdEnabled(command)) {
    return;
  }
  if (auto* action = FindAction(command)) {
    action->InvokeAction();
  } else if (IsLayoutCommand(command) ||
             (command >= kCompact && command <= kToggleEssential)) {
    dispatch_.Run(static_cast<Command>(command));
  }
}
bool SidebarMenuModel::IsItemDynamicAt(size_t index) const {
  return IsItemForCommandIdDynamic(GetCommandIdAt(index));
}
bool SidebarMenuModel::IsItemForCommandIdDynamic(int command) const {
  return command == kToggleEssential || FindAction(command) != nullptr;
}
std::u16string SidebarMenuModel::GetLabelForCommandId(int command) const {
  if (command == kToggleEssential) {
    const auto state = read_state_.Run();
    return state && state->essential ? u"Remove current tab from Essentials"
                                     : u"Add current tab to Essentials";
  }
  const auto found = std::ranges::find(actions_, command, &Action::command);
  if (found != actions_.end()) {
    return found->source && !found->source->GetText().empty()
               ? Label(*found->source)
               : found->fallback_label;
  }
  return {};
}
std::u16string SidebarMenuModel::GetLabelAt(size_t index) const {
  return IsItemDynamicAt(index) ? GetLabelForCommandId(GetCommandIdAt(index))
                                : ui::SimpleMenuModel::GetLabelAt(index);
}
ui::ImageModel SidebarMenuModel::GetIconForCommandId(int command) const {
  if (const auto* action = FindAction(command))
    return action->GetImage();
  return {};
}
ui::ImageModel SidebarMenuModel::GetIconAt(size_t index) const {
  if (const auto* action = FindAction(GetCommandIdAt(index))) {
    return action->GetImage();
  }
  return ui::SimpleMenuModel::GetIconAt(index);
}
std::string_view SidebarMenuModel::Glyph(int command) {
  switch (command) {
    case kCompact:
      return "\uE73F";
    case kNewWorkspace:
      return "\uE710";
    case kSidebarRight:
      return "\uE8A0";
    case kAppearance:
      return "\uE713";
    case kEditTheme:
      return "\uE790";
    case kNewFolder:
      return "\uE8F4";
    case kToggleEssential:
      return "\uE734";
    case kAppearanceMenu:
      return "\uE790";
    case kPageMenu:
      return "\uE8A5";
    default:
      return {};
  }
}
}  // namespace krypton_ui
