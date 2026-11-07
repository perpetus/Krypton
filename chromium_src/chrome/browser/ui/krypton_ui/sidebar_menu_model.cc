// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/sidebar_menu_model.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_util.h"
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
                                   Dispatch dispatch)
    : ui::SimpleMenuModel(this),
      actions_(actions.begin(), actions.end()),
      read_state_(std::move(read_state)),
      can_execute_(std::move(can_execute)),
      dispatch_(std::move(dispatch)) {
  AddCheckItem(kCompact, u"Enable compact mode");
  AddSeparator(ui::NORMAL_SEPARATOR);
  for (const auto& action : actions_) {
    if (action.source) {
      AddItem(action.command, action.source->GetText().empty()
                                  ? action.fallback_label
                                  : Label(*action.source));
    }
  }
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItem(kNewWorkspace, u"New workspace\u2026");
  AddCheckItem(kSidebarRight, u"Tabs on the right");
  AddItem(kAppearance, u"Appearance settings\u2026");
  AddItem(kEditTheme, u"Edit theme\u2026");
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
  switch (command) {
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
  switch (command) {
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
  return command >= kCompact && command <= kEditTheme;
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
  } else if (command >= kCompact && command <= kEditTheme) {
    dispatch_.Run(static_cast<Command>(command));
  }
}
bool SidebarMenuModel::IsItemDynamicAt(size_t index) const {
  return FindAction(GetCommandIdAt(index)) != nullptr;
}
std::u16string SidebarMenuModel::GetLabelAt(size_t index) const {
  const auto found =
      std::ranges::find(actions_, GetCommandIdAt(index), &Action::command);
  if (found != actions_.end()) {
    return found->source && !found->source->GetText().empty()
               ? Label(*found->source)
               : found->fallback_label;
  }
  return ui::SimpleMenuModel::GetLabelAt(index);
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
    default:
      return {};
  }
}
}  // namespace krypton_ui
