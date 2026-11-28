// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/shortcut_settings.h"

#include <algorithm>
#include <climits>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/accelerator_table.h"
#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_metadata.h"
#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_serialization.h"
#include "chrome/browser/ui/krypton_ui/layout_prefs.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/pref_service.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/events/event_constants.h"

namespace krypton_ui {
namespace {

using browser_shortcuts::AcceleratorFromString;
using browser_shortcuts::AcceleratorToDisplayString;
using browser_shortcuts::AcceleratorToString;
using browser_shortcuts::GetBrowserShortcutCommandLabel;
using Bindings = std::map<ui::Accelerator, AcceleratorMapping>;

struct NativeCommand {
  int id;
  int label;
  ui::KeyboardCode key;
};

constexpr NativeCommand kNativeCommands[] = {
    {kShortcutSplitGrid, IDS_KRYPTON_SHORTCUT_SPLIT_GRID, ui::VKEY_G},
    {kShortcutSplitColumns, IDS_SPLIT_TAB_NEW_VERTICAL, ui::VKEY_V},
    {kShortcutSplitRows, IDS_SPLIT_TAB_NEW_HORIZONTAL, ui::VKEY_H},
    {kShortcutUnsplit, IDS_SPLIT_TAB_SEPARATE_VIEWS, ui::VKEY_U},
    {kShortcutSleepTab, IDS_KRYPTON_SHORTCUT_SLEEP_TAB, ui::VKEY_UNKNOWN},
    {kShortcutNewNote, IDS_KRYPTON_SHORTCUT_NEW_NOTE, ui::VKEY_UNKNOWN},
    {kShortcutNewEasel, IDS_KRYPTON_SHORTCUT_NEW_EASEL, ui::VKEY_UNKNOWN},
};

constexpr KrShortcutEditResult kAccepted{KR_SHORTCUT_EDIT_OK, 0};
constexpr KrShortcutEditResult kRejected{KR_SHORTCUT_EDIT_REJECTED, 0};

const NativeCommand *FindNativeCommand(int id) {
  const auto found = std::ranges::find(kNativeCommands, id, &NativeCommand::id);
  return found == std::end(kNativeCommands) ? nullptr : found;
}

int Group(int command) {
  if (FindNativeCommand(command)) {
    return IDS_KRYPTON_SHORTCUT_GROUP_KRYPTON;
  }
  if (command >= 33000 && command < 34000) {
    return IDS_NTP_CUSTOMIZE_TOOLBAR_CATEGORY_NAVIGATION;
  }
  if (command >= 34000 && command < 35000) {
    return IDS_KRYPTON_SHORTCUT_GROUP_TABS;
  }
  return IDS_NTP_CUSTOMIZE_TOOLBAR_CATEGORY_TOOLS_AND_ACTIONS;
}

std::string PrefKey(int command) {
  // IDC values are explicit identifiers and are independent of catalog order.
  return base::NumberToString(command);
}

bool IsEditable(int command, const PrefService &profile_prefs, bool native) {
  if (FindNativeCommand(command)) {
    return native;
  }
  return GetBrowserShortcutCommandLabel(command, &profile_prefs) &&
         browser_shortcuts::IsBrowserShortcutDefaultUserModifiable(command);
}

Bindings BuildBindings(const std::vector<AcceleratorMapping> &mappings,
                       const base::DictValue &overrides,
                       const PrefService &profile_prefs, bool native) {
  Bindings bindings;
  for (const auto &entry : mappings) {
    bindings.insert_or_assign(ui::Accelerator(entry.keycode, entry.modifiers),
                              entry);
  }
  if (native) {
    for (const auto &entry : kNativeCommands) {
      if (entry.key != ui::VKEY_UNKNOWN) {
        constexpr int modifiers = ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN;
        bindings.insert_or_assign(
            ui::Accelerator(entry.key, modifiers),
            AcceleratorMapping{entry.key, modifiers, entry.id, std::nullopt});
      }
    }
  }

  // Release all overridden defaults before assigning new chords. This also
  // makes changes to two commands visible as a single preference update.
  std::vector<std::pair<int, std::optional<ui::Accelerator>>> edits;
  for (auto [key, value] : overrides) {
    int command;
    if (!base::StringToInt(key, &command) ||
        !IsEditable(command, profile_prefs, native) || !value.is_string()) {
      continue;
    }
    const auto &chord = value.GetString();
    auto accelerator = AcceleratorFromString(chord);
    if (chord.empty() || accelerator) {
      edits.emplace_back(command, accelerator);
    }
  }
  for (const auto &[command, accelerator] : edits) {
    std::erase_if(bindings, [command](const auto &item) {
      return item.second.command_id == command;
    });
  }
  for (const auto &[command, accelerator] : edits) {
    if (accelerator) {
      bindings.try_emplace(*accelerator,
                           AcceleratorMapping{accelerator->key_code(),
                                              accelerator->modifiers(), command,
                                              std::nullopt});
    }
  }
  return bindings;
}

std::optional<ui::Accelerator> DecodeChord(KrShortcutChord chord) {
  constexpr uint32_t allowed =
      KR_SHORTCUT_CTRL | KR_SHORTCUT_ALT | KR_SHORTCUT_SHIFT;
  if (chord.key > 0xff || (chord.modifiers & ~allowed)) {
    return std::nullopt;
  }

  const bool ctrl = chord.modifiers & KR_SHORTCUT_CTRL;
  const bool alt = chord.modifiers & KR_SHORTCUT_ALT;
  const bool shift = chord.modifiers & KR_SHORTCUT_SHIFT;
  const auto key = static_cast<ui::KeyboardCode>(chord.key);
  const bool function_key = key >= ui::VKEY_F1 && key <= ui::VKEY_F24;
  if ((!function_key && !ctrl && !alt) ||
      (alt && (key == ui::VKEY_TAB || key == ui::VKEY_ESCAPE ||
               key == ui::VKEY_SPACE || key == ui::VKEY_F4 ||
               (ctrl && key == ui::VKEY_DELETE))) ||
      (ctrl && key == ui::VKEY_ESCAPE) || (key == ui::VKEY_F10 && shift) ||
      key == ui::VKEY_SNAPSHOT) {
    return std::nullopt;
  }
  if (ctrl && !alt &&
      (key == ui::VKEY_A || key == ui::VKEY_C || key == ui::VKEY_V ||
       key == ui::VKEY_X || key == ui::VKEY_Y || key == ui::VKEY_Z)) {
    return std::nullopt;
  }

  const ui::Accelerator accelerator(key, (ctrl ? ui::EF_CONTROL_DOWN : 0) |
                                             (alt ? ui::EF_ALT_DOWN : 0) |
                                             (shift ? ui::EF_SHIFT_DOWN : 0));
  const auto serialized = AcceleratorToString(accelerator);
  if (serialized.empty() || AcceleratorFromString(serialized) != accelerator) {
    return std::nullopt;
  }
  return accelerator;
}

struct CatalogEntry {
  uint32_t command;
  int group;
  std::string label;
  std::vector<std::string> defaults;
  std::string current;
};

KrStr Borrow(const std::string &text) { return {text.data(), text.size()}; }

} // namespace

void ApplyShortcutOverrides(std::vector<AcceleratorMapping> &mappings,
                            const PrefService &local_state,
                            const PrefService &profile_prefs,
                            bool has_native_shell) {
  const auto bindings =
      BuildBindings(mappings, local_state.GetDict(kShortcutOverridesPref),
                    profile_prefs, has_native_shell);
  mappings.clear();
  for (const auto &[accelerator, mapping] : bindings) {
    mappings.push_back(mapping);
  }
}

void PublishShortcutCatalog(const KrShellUI &ui, const PrefService &local_state,
                            const PrefService &profile_prefs) {
  const auto table = GetAcceleratorList();
  const auto defaults = BuildBindings(table, {}, profile_prefs, true);
  const auto current = BuildBindings(
      table, local_state.GetDict(kShortcutOverridesPref), profile_prefs, true);
  std::map<int, std::vector<std::string>> default_text;
  std::map<int, std::vector<std::string>> current_text;
  for (const auto &[accelerator, mapping] : defaults) {
    default_text[mapping.command_id].push_back(
        base::UTF16ToUTF8(AcceleratorToDisplayString(accelerator)));
  }
  for (const auto &[accelerator, mapping] : current) {
    current_text[mapping.command_id].push_back(
        base::UTF16ToUTF8(AcceleratorToDisplayString(accelerator)));
  }

  for (int command :
       browser_shortcuts::GetBrowserShortcutCommandsWithoutDefault(
           &profile_prefs)) {
    default_text.try_emplace(command);
  }
  for (const auto &command : kNativeCommands) {
    default_text.try_emplace(command.id);
  }

  std::vector<CatalogEntry> entries;
  for (auto &[id, chords] : default_text) {
    if (!IsEditable(id, profile_prefs, true)) {
      continue;
    }
    std::u16string label;
    if (const auto *command = FindNativeCommand(id)) {
      label = l10n_util::GetStringUTF16(command->label);
    } else if (const auto browser_command =
                   GetBrowserShortcutCommandLabel(id, &profile_prefs)) {
      label = browser_command->replacement
                  ? l10n_util::GetStringFUTF16(browser_command->message_id,
                                               *browser_command->replacement)
                  : l10n_util::GetStringUTF16(browser_command->message_id);
    }
    std::erase(label, u'&');
    entries.push_back({static_cast<uint32_t>(id), Group(id),
                       base::UTF16ToUTF8(label), std::move(chords),
                       base::JoinString(current_text[id], " / ")});
  }
  std::ranges::sort(entries, [](const auto &left, const auto &right) {
    return std::tie(left.group, left.label, left.command) <
           std::tie(right.group, right.label, right.command);
  });

  std::map<int, std::string> groups;
  std::vector<std::vector<KrStr>> default_views(entries.size());
  std::vector<KrShortcutCommand> records;
  records.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &entry = entries[i];
    if (!groups.contains(entry.group)) {
      groups.emplace(entry.group, l10n_util::GetStringUTF8(entry.group));
    }
    auto &views = default_views[i];
    for (const auto &text : entry.defaults) {
      views.push_back(Borrow(text));
    }
    records.push_back({sizeof(KrShortcutCommand), entry.command,
                       Borrow(groups.at(entry.group)), Borrow(entry.label),
                       views.data(), views.size(), Borrow(entry.current)});
  }
  ui.set_shortcuts(ui.self, records.data(), records.size(),
                   sizeof(KrShortcutCommand));
}

KrShortcutEditResult SetShortcut(PrefService &local_state,
                                 const PrefService &profile_prefs,
                                 uint32_t command, KrShortcutChord chord,
                                 uint32_t replace_conflict) {
  if (command > INT_MAX || !IsEditable(command, profile_prefs, true)) {
    return kRejected;
  }
  const auto accelerator = DecodeChord(chord);
  if (!accelerator) {
    return kRejected;
  }

  auto overrides = local_state.GetDict(kShortcutOverridesPref).Clone();
  const auto bindings =
      BuildBindings(GetAcceleratorList(), overrides, profile_prefs, true);
  const auto found = bindings.find(*accelerator);
  const int conflict = found != bindings.end() && found->second.command_id !=
                                                      static_cast<int>(command)
                           ? found->second.command_id
                           : 0;
  if (conflict && !IsEditable(conflict, profile_prefs, true)) {
    return kRejected;
  }
  if (conflict && static_cast<uint32_t>(conflict) != replace_conflict) {
    return {KR_SHORTCUT_EDIT_CONFLICT, static_cast<uint32_t>(conflict)};
  }
  if (!conflict && replace_conflict) {
    return kRejected;
  }
  if (conflict) {
    overrides.Set(PrefKey(conflict), "");
  }
  overrides.Set(PrefKey(command), AcceleratorToString(*accelerator));
  local_state.SetDict(kShortcutOverridesPref, std::move(overrides));
  return kAccepted;
}

KrShortcutEditResult UnassignShortcut(PrefService &local_state,
                                      const PrefService &profile_prefs,
                                      uint32_t command) {
  if (command > INT_MAX || !IsEditable(command, profile_prefs, true)) {
    return kRejected;
  }
  auto overrides = local_state.GetDict(kShortcutOverridesPref).Clone();
  overrides.Set(PrefKey(command), "");
  local_state.SetDict(kShortcutOverridesPref, std::move(overrides));
  return kAccepted;
}

KrShortcutEditResult ResetShortcut(PrefService &local_state,
                                   const PrefService &profile_prefs,
                                   uint32_t command) {
  if (!command) {
    local_state.ClearPref(kShortcutOverridesPref);
    return kAccepted;
  }
  if (command > INT_MAX || !IsEditable(command, profile_prefs, true)) {
    return kRejected;
  }
  auto overrides = local_state.GetDict(kShortcutOverridesPref).Clone();
  const auto table = GetAcceleratorList();
  const auto defaults = BuildBindings(table, {}, profile_prefs, true);
  const auto current = BuildBindings(table, overrides, profile_prefs, true);
  overrides.Remove(PrefKey(command));
  for (const auto &[chord, entry] : defaults) {
    const auto owner = current.find(chord);
    if (entry.command_id == static_cast<int>(command) &&
        owner != current.end() &&
        owner->second.command_id != entry.command_id) {
      overrides.Set(PrefKey(owner->second.command_id), "");
    }
  }
  local_state.SetDict(kShortcutOverridesPref, std::move(overrides));
  return kAccepted;
}

} // namespace krypton_ui
