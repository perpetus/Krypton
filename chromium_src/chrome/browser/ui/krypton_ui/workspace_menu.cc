// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/workspace_menu.h"

#include <algorithm>
#include <utility>

#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace krypton_ui {
namespace {
constexpr int kCreate = 1;
constexpr int kEdit = 2;
constexpr int kMove = 3;
constexpr int kTheme = 4;
constexpr int kFirstWorkspace = 100;
std::u16string Label(const Workspace& space) {
  auto label = base::UTF8ToUTF16(space.name);
  // User names are literal, including ampersands.
  base::ReplaceSubstringsAfterOffset(&label, 0, u"&", u"&&");
  return label;
}
}  // namespace

WorkspaceTabMenuModel::WorkspaceTabMenuModel(const WorkspaceCatalog& catalog,
                                             KrSpaceId current,
                                             CanMove can_move,
                                             Move move)
    : ui::SimpleMenuModel(this),
      can_move_(std::move(can_move)),
      move_(std::move(move)) {
  int command = kSubmenu + 1;
  for (const auto& space : catalog.spaces) {
    if (space.id != current) {
      AddItem(command, Label(space));
      entries_.push_back({command++, space.id});
    }
  }
}
WorkspaceTabMenuModel::~WorkspaceTabMenuModel() = default;
bool WorkspaceTabMenuModel::HasCommand(int command) const {
  return command == kSubmenu ||
         std::ranges::find(entries_, command, &Entry::command) !=
             entries_.end();
}
bool WorkspaceTabMenuModel::IsCommandIdEnabled(int command) const {
  if (command == kSubmenu) {
    return std::ranges::any_of(entries_, [this](const Entry& entry) {
      return can_move_.Run(entry.space);
    });
  }
  const auto found = std::ranges::find(entries_, command, &Entry::command);
  return found != entries_.end() && can_move_.Run(found->space);
}
void WorkspaceTabMenuModel::ExecuteCommand(int command, int event_flags) {
  const auto found = std::ranges::find(entries_, command, &Entry::command);
  if (found != entries_.end() && can_move_.Run(found->space)) {
    move_.Run(found->space);
  }
}

WorkspaceMenuModel::WorkspaceMenuModel(const WorkspaceCatalog& catalog,
                                       KrSpaceId active,
                                       KrSpaceId target,
                                       bool can_move_tab,
                                       Dispatch dispatch,
                                       bool can_edit_theme)
    : ui::SimpleMenuModel(this),
      active_(active),
      dispatch_(std::move(dispatch)),
      can_edit_theme_(can_edit_theme) {
  int command = kFirstWorkspace;
  for (const auto& space : catalog.spaces) {
    AddRadioItem(command, Label(space), 0);
    entries_.push_back({command++, Operation::kSwitch, space.id});
  }
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItem(kCreate, u"New workspace\u2026");
  entries_.push_back({kCreate, Operation::kCreate, 0});
  const auto editable = catalog.Contains(target) ? target : active;
  AddItem(kEdit, u"Edit workspace\u2026");
  entries_.push_back({kEdit, Operation::kEdit, editable});
  AddItem(kTheme, u"Edit theme\u2026");
  entries_.push_back({kTheme, Operation::kTheme, editable});
  if (can_move_tab && catalog.spaces.size() > 1) {
    move_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
    for (const auto& space : catalog.spaces) {
      if (space.id == active)
        continue;
      move_menu_->AddItem(command, Label(space));
      entries_.push_back({command++, Operation::kMoveTab, space.id});
    }
    AddSubMenu(kMove, u"Move current tab to", move_menu_.get());
  }
}
WorkspaceMenuModel::~WorkspaceMenuModel() = default;
bool WorkspaceMenuModel::IsCommandIdChecked(int command) const {
  const auto found = std::ranges::find(entries_, command, &Entry::command);
  return found != entries_.end() && found->operation == Operation::kSwitch &&
         found->space == active_;
}
bool WorkspaceMenuModel::IsCommandIdEnabled(int command) const {
  const auto found = std::ranges::find(entries_, command, &Entry::command);
  return command == kMove ||
         (found != entries_.end() &&
          (found->operation != Operation::kTheme || can_edit_theme_));
}
void WorkspaceMenuModel::ExecuteCommand(int command, int event_flags) {
  const auto found = std::ranges::find(entries_, command, &Entry::command);
  if (found != entries_.end() && IsCommandIdEnabled(command))
    dispatch_.Run(found->operation, found->space);
}
std::string_view WorkspaceMenuModel::Glyph(int command) {
  switch (command) {
    case kCreate:
      return "\uE710";
    case kEdit:
      return "\uE70F";
    case kMove:
      return "\uE8AB";
    case kTheme:
      return "\uE790";
    default:
      return {};
  }
}
}  // namespace krypton_ui
