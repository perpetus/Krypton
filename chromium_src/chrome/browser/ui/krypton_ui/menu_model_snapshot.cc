// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/menu_model_snapshot.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/models/button_menu_item_model.h"
#include "ui/base/models/image_model.h"
#include "ui/base/models/menu_model.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/font_render_params.h"

namespace krypton_ui {
namespace {

KrStr Borrow(const std::string& text) {
  return {text.data(), text.size()};
}

KrKeyboardAccelerator CaptureAccelerator(const ui::Accelerator& key) {
  return {sizeof(KrKeyboardAccelerator),
          static_cast<uint32_t>(key.key_code()),
          key.modifiers(),
          static_cast<uint32_t>(key.key_state()),
          key.time_stamp().since_origin().InMicroseconds(),
          key.source_device_id(),
          key.interrupted_by_mouse_event()};
}

uint32_t ItemType(ui::MenuModel::ItemType type) {
  switch (type) {
    case ui::MenuModel::TYPE_CHECK:
      return KR_MENU_ITEM_CHECK;
    case ui::MenuModel::TYPE_RADIO:
      return KR_MENU_ITEM_RADIO;
    case ui::MenuModel::TYPE_SEPARATOR:
      return KR_MENU_ITEM_SEPARATOR;
    case ui::MenuModel::TYPE_SUBMENU:
    case ui::MenuModel::TYPE_ACTIONABLE_SUBMENU:
      return KR_MENU_ITEM_SUBMENU;
    case ui::MenuModel::TYPE_TITLE:
      return KR_MENU_ITEM_TITLE;
    case ui::MenuModel::TYPE_BUTTON_ITEM:
      return KR_MENU_ITEM_CONTROLS;
    case ui::MenuModel::TYPE_COMMAND:
    case ui::MenuModel::TYPE_HIGHLIGHTED:
      return KR_MENU_ITEM_COMMAND;
  }
  return KR_MENU_ITEM_COMMAND;
}

bool IsCommand(int type) {
  return type == ui::MenuModel::TYPE_COMMAND ||
         type == ui::MenuModel::TYPE_CHECK ||
         type == ui::MenuModel::TYPE_RADIO ||
         type == ui::MenuModel::TYPE_HIGHLIGHTED ||
         type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU;
}

}  // namespace

MenuModelSnapshot::MenuModelSnapshot(
    PublishImage images,
    base::RepeatingClosure invalidated,
    base::RepeatingCallback<void(KrImageId)> release_image)
    : images_(std::move(images)),
      release_image_(std::move(release_image)),
      invalidated_(std::move(invalidated)) {
  CHECK(images_);
}

MenuModelSnapshot::~MenuModelSnapshot() {
  Clear();
}

void MenuModelSnapshot::Clear() {
  weak_factory_.InvalidateWeakPtrs();
  valid_ = false;
  detaching_ = true;
  for (const auto& model : models_) {
    if (model && model->menu_model_delegate() == this) {
      model->SetMenuModelDelegate(nullptr);
    }
  }
  models_.clear();
  data_.clear();
  menus_.clear();
  commands_.clear();
  if (release_image_) {
    for (const auto id : image_leases_) {
      release_image_.Run(id);
    }
  }
  image_leases_.clear();
  next_command_ = 0;
  published_request_.reset();
  detaching_ = false;
}

base::expected<void, std::string> MenuModelSnapshot::Capture(
    ui::MenuModel& model,
    const ui::ColorProvider* colors,
    GlyphTable glyphs,
    base::span<const int> root_commands,
    KeepOpen keep_open) {
  Clear();
  const base::AutoReset<raw_ptr<const ui::ColorProvider>> scoped_colors(
      &colors_, colors);
  const base::AutoReset<GlyphTable> scoped_glyphs(&glyphs_, glyphs);
  const base::AutoReset<KeepOpen> scoped_keep_open(&keep_open_, keep_open);
  const base::AutoReset<base::span<const int>> scoped_commands(&root_commands_,
                                                               root_commands);
  std::vector<SourceItem> path;
  const auto root = CaptureMenu(model, path);
  if (!root.has_value()) {
    auto error = root.error();
    Clear();
    return base::unexpected(std::move(error));
  }
  menus_.reserve(data_.size());
  for (auto& menu : data_) {
    menu.items.reserve(menu.data.size());
    for (auto& item : menu.data) {
      item.Bind();
      menu.items.push_back(item.value);
    }
    menus_.push_back({sizeof(KrMenu), menu.items.data(), menu.items.size(),
                      sizeof(KrMenuItem)});
  }
  for (const auto& source : models_) {
    if (!source || source->menu_model_delegate()) {
      Clear();
      return base::unexpected("Menu model already has a presentation delegate");
    }
    source->SetMenuModelDelegate(this);
  }
  valid_ = true;
  return {};
}

base::span<const KrMenu> MenuModelSnapshot::menus() const {
  return valid_ ? base::span(menus_) : base::span<const KrMenu>();
}

base::expected<void, std::string> MenuModelSnapshot::Publish(
    const KrShellUI& ui,
    uint32_t request_id,
    bool panel,
    base::span<const PanelRow> layout) {
  if (!valid_ || !KR_HAS_SLOT(&ui, KrShellUI, show_menu) || !ui.show_menu) {
    return base::unexpected("Cannot publish an invalid menu snapshot or shell");
  }
  if (published_request_ != request_id) {
    weak_factory_.InvalidateWeakPtrs();
    published_request_ = request_id;
  }
  const bool tiles = panel && !layout.empty() &&
                     KR_HAS_SLOT(&ui, KrShellUI, show_menu_panel_with_tiles) &&
                     ui.show_menu_panel_with_tiles;
  if (!data_.empty()) {
    for (size_t index = 0; index < data_[0].items.size(); ++index) {
      auto& item = data_[0].items[index];
      item.type = data_[0].data[index].value.type;
      if (tiles) {
        const auto found =
            std::ranges::find(layout, item.metadata->source_command_id,
                              &PanelRow::source_command);
        if (found != layout.end())
          item.type = found->type;
      }
    }
  }
  if (tiles) {
    ui.show_menu_panel_with_tiles(ui.self, request_id, menus_.data(),
                                  menus_.size(), sizeof(KrMenu));
  } else if (panel && KR_HAS_SLOT(&ui, KrShellUI, show_menu_panel) &&
             ui.show_menu_panel) {
    ui.show_menu_panel(ui.self, request_id, menus_.data(), menus_.size(),
                       sizeof(KrMenu));
  } else {
    ui.show_menu(ui.self, request_id, menus_.data(), menus_.size(),
                 sizeof(KrMenu));
  }
  return {};
}

base::expected<int32_t, std::string> MenuModelSnapshot::CaptureMenu(
    ui::MenuModel& model,
    std::vector<SourceItem>& path) {
  for (const auto& ancestor : path) {
    if (ancestor.model.get() == &model) {
      return base::unexpected("Cyclic source menu model");
    }
  }
  if (data_.size() >=
      static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return base::unexpected("Menu count exceeds the ABI index range");
  }
  const auto menu = static_cast<int32_t>(data_.size());
  data_.emplace_back();
  const auto source = model.AsWeakPtr();
  if (!std::ranges::any_of(models_, [&model](const auto& existing) {
        return existing.get() == &model;
      })) {
    models_.push_back(source);
  }
  const auto count = model.GetItemCount();
  data_[menu].data.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    if (path.empty() && !root_commands_.empty() &&
        !std::ranges::contains(root_commands_, model.GetCommandIdAt(index)))
      continue;
    auto captured = CaptureItem(model, index);
    if (!captured.has_value()) {
      return base::unexpected(captured.error());
    }
    auto item = std::move(*captured);
    const auto type = model.GetTypeAt(index);
    path.push_back({source, index, model.GetCommandIdAt(index), type});
    if (type == ui::MenuModel::TYPE_SUBMENU ||
        type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU) {
      auto* child = model.GetSubmenuModelAt(index);
      if (!child) {
        return base::unexpected("Source submenu has no model");
      }
      const auto submenu = CaptureMenu(*child, path);
      if (!submenu.has_value()) {
        return base::unexpected(submenu.error());
      }
      item.value.submenu = *submenu;
    } else if (type == ui::MenuModel::TYPE_BUTTON_ITEM) {
      const auto buttons = CaptureButtons(model, index, path);
      if (!buttons.has_value()) {
        return base::unexpected(buttons.error());
      }
      item.value.submenu = *buttons;
      item.text[kButtonRowLabel] =
          base::UTF16ToUTF8(model.GetButtonMenuItemAt(index)->label());
    }
    if (IsCommand(type)) {
      AddCommand(item, {path, std::nullopt});
    }
    path.pop_back();
    data_[menu].data.push_back(std::move(item));
  }
  return menu;
}

base::expected<MenuModelSnapshot::ItemData, std::string>
MenuModelSnapshot::CaptureItem(ui::MenuModel& model, size_t index) {
  ItemData result;
  const auto type = model.GetTypeAt(index);
  result.value.struct_size = sizeof(KrMenuItem);
  result.value.type = ItemType(type);
  result.value.command_id = model.GetCommandIdAt(index);
  result.value.submenu = -1;
  result.value.flags =
      (model.IsEnabledAt(index) ? KR_MENU_ITEM_ENABLED : 0) |
      (model.IsItemCheckedAt(index) ? KR_MENU_ITEM_CHECKED : 0);
  result.text[kLabel] = base::UTF16ToUTF8(model.GetLabelAt(index));
  result.text[kSecondary] = base::UTF16ToUTF8(model.GetSecondaryLabelAt(index));
  result.text[kMinor] = base::UTF16ToUTF8(model.GetMinorTextAt(index));
  result.text[kAccessible] =
      base::UTF16ToUTF8(model.GetAccessibleNameAt(index));
  result.text[kElement] = model.GetElementIdentifierAt(index).GetName();
  if (glyphs_) {
    result.text[kGlyph] = glyphs_(result.value.command_id);
  }
  auto& metadata = result.metadata;
  metadata.struct_size = sizeof(KrMenuItemMetadata);
  metadata.source_type = type;
  metadata.source_command_id = result.value.command_id;
  if (keep_open_ && keep_open_(metadata.source_command_id))
    result.value.flags |= KR_MENU_ITEM_KEEP_OPEN;
  metadata.source_index = index;
  metadata.source_button_type = -1;
  metadata.radio_group = model.GetGroupIdAt(index);
  metadata.separator_type = model.GetSeparatorTypeAt(index);
  const auto badge = model.GetNewBadgeTypeAt(index);
  metadata.badge_type = badge ? (*badge == ui::NewBadgeType::kNew ? 1 : 2) : 0;
  metadata.flags =
      (model.IsVisibleAt(index) ? KR_MENU_META_VISIBLE : 0) |
      (model.IsItemDynamicAt(index) ? KR_MENU_META_DYNAMIC : 0) |
      (model.MayHaveMnemonicsAt(index) ? KR_MENU_META_MAY_HAVE_MNEMONICS : 0) |
      (model.GetMinorTextIsUrlAt(index) ? KR_MENU_META_MINOR_TEXT_IS_URL : 0) |
      (model.GetMinorIconOnRight(ui::MenuModel::MinorIconOnRightPasskey(index))
           ? KR_MENU_META_MINOR_ICON_ON_RIGHT
           : 0) |
      (model.GetForceShowAcceleratorForItemAt(index)
           ? KR_MENU_META_FORCE_ACCELERATOR
           : 0) |
      (model.IsAlertedAt(index) ? KR_MENU_META_ALERTED : 0) |
      (model.IsNewFeatureAt(index) ? KR_MENU_META_NEW_FEATURE : 0) |
      (type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU
           ? KR_MENU_META_ACTIONABLE_SUBMENU
           : 0);
  ui::Accelerator accelerator;
  if (model.GetAcceleratorAt(index, &accelerator)) {
    result.accelerator = CaptureAccelerator(accelerator);
    result.text[kAccelerator] =
        base::UTF16ToUTF8(accelerator.GetShortcutText());
  }
  if (const auto* list = model.GetLabelFontListAt(index)) {
    metadata.flags |= KR_MENU_META_CUSTOM_FONT;
    metadata.font_style = list->GetFontStyle();
    metadata.font_size_pixels = list->GetFontSize();
    metadata.font_weight = static_cast<int32_t>(list->GetFontWeight());
    metadata.font_height = list->GetHeight();
    metadata.font_baseline = list->GetBaseline();
    metadata.font_cap_height = list->GetCapHeight();
    result.fonts.reserve(list->GetFonts().size());
    for (const auto& font : list->GetFonts()) {
      FontData data;
      data.family = font.GetFontName();
      data.actual_families = font.GetActualFontNames();
      const auto& params = font.GetFontRenderParams();
      data.value = {
          .struct_size = sizeof(KrMenuFont),
          .size_pixels = font.GetFontSize(),
          .style = font.GetStyle(),
          .weight = static_cast<int32_t>(font.GetWeight()),
          .height = font.GetHeight(),
          .baseline = font.GetBaseline(),
          .cap_height = font.GetCapHeight(),
          .render_flags =
              (params.antialiasing ? KR_MENU_FONT_ANTIALIASING : 0u) |
              (params.subpixel_positioning ? KR_MENU_FONT_SUBPIXEL_POSITIONING
                                           : 0u) |
              (params.autohinter ? KR_MENU_FONT_AUTOHINTER : 0u) |
              (params.use_bitmaps ? KR_MENU_FONT_USE_BITMAPS : 0u),
          .hinting = static_cast<uint32_t>(params.hinting),
          .subpixel_rendering =
              static_cast<uint32_t>(params.subpixel_rendering),
          .text_contrast = params.text_contrast,
          .text_gamma = params.text_gamma};
      result.fonts.push_back(std::move(data));
    }
  }
  const std::array colors{model.GetForegroundColorId(index),
                          model.GetSubmenuBackgroundColorId(index),
                          model.GetSelectedBackgroundColorId(index)};
  const std::array<uint32_t, 3> roles{KR_MENU_COLOR_FOREGROUND,
                                      KR_MENU_COLOR_SUBMENU_BACKGROUND,
                                      KR_MENU_COLOR_SELECTED_BACKGROUND};
  for (size_t role = 0; role < colors.size(); ++role) {
    if (colors[role]) {
      result.colors.push_back({sizeof(KrMenuColor), roles[role],
                               static_cast<int32_t>(*colors[role]),
                               colors_ != nullptr,
                               colors_ ? colors_->GetColor(*colors[role]) : 0});
    }
  }
  const auto images = CaptureImages(model, index, result);
  if (!images.has_value()) {
    return base::unexpected(images.error());
  }
  return result;
}

base::expected<void, std::string> MenuModelSnapshot::CaptureImages(
    ui::MenuModel& model,
    size_t index,
    ItemData& output) {
  const auto main = model.GetIconAt(index);
  if (!main.IsEmpty()) {
    const auto image = images_.Run(main);
    if (!image.has_value()) {
      return base::unexpected(image.error());
    }
    if (*image == 0) {
      return base::unexpected("Source menu icon was not published");
    }
    output.value.icon = *image;
    image_leases_.push_back(*image);
  }
  const auto minor = model.GetMinorIconAt(index);
  if (!minor.IsEmpty()) {
    const auto image = images_.Run(minor);
    if (!image.has_value()) {
      return base::unexpected(image.error());
    }
    if (*image == 0) {
      return base::unexpected("Source minor menu icon was not published");
    }
    output.metadata.minor_icon = *image;
    image_leases_.push_back(*image);
  }
  return {};
}

base::expected<int32_t, std::string> MenuModelSnapshot::CaptureButtons(
    ui::MenuModel& model,
    size_t index,
    const std::vector<SourceItem>& path) {
  auto* buttons = model.GetButtonMenuItemAt(index);
  if (!buttons || data_.size() >= static_cast<size_t>(
                                      std::numeric_limits<int32_t>::max())) {
    return base::unexpected("Invalid source button menu");
  }
  const auto menu = static_cast<int32_t>(data_.size());
  data_.emplace_back();
  for (size_t button = 0; button < buttons->GetItemCount(); ++button) {
    ItemData item;
    item.value.struct_size = sizeof(KrMenuItem);
    item.value.command_id = buttons->GetCommandIdAt(button);
    item.value.submenu = -1;
    const auto type = buttons->GetTypeAt(button);
    item.value.type = type == ui::ButtonMenuItemModel::TYPE_SPACE
                          ? KR_MENU_ITEM_SEPARATOR
                      : type == ui::ButtonMenuItemModel::TYPE_BUTTON_LABEL
                          ? KR_MENU_ITEM_TITLE
                          : KR_MENU_ITEM_COMMAND;
    const bool actionable = type == ui::ButtonMenuItemModel::TYPE_BUTTON;
    item.value.flags =
        actionable && buttons->IsEnabledAt(button) ? KR_MENU_ITEM_ENABLED : 0;
    if (actionable && !buttons->DismissesMenuAt(button)) {
      item.value.flags |= KR_MENU_ITEM_KEEP_OPEN;
    }
    item.metadata.struct_size = sizeof(KrMenuItemMetadata);
    item.metadata.source_type = ui::MenuModel::TYPE_BUTTON_ITEM;
    item.metadata.source_command_id = item.value.command_id;
    item.metadata.source_index = index;
    item.metadata.source_button_type = type;
    item.metadata.source_button_index = button;
    item.metadata.flags =
        KR_MENU_META_VISIBLE |
        (type != ui::ButtonMenuItemModel::TYPE_SPACE &&
                 buttons->IsItemDynamicAt(button)
             ? KR_MENU_META_DYNAMIC
             : 0) |
        (buttons->PartOfGroup(button) ? KR_MENU_META_BUTTON_GROUP_MEMBER : 0);
    if (type != ui::ButtonMenuItemModel::TYPE_SPACE) {
      item.text[kLabel] = base::UTF16ToUTF8(buttons->GetLabelAt(button));
    }
    item.text[kButtonRowLabel] = base::UTF16ToUTF8(buttons->label());
    if (glyphs_ && actionable) {
      item.text[kGlyph] = glyphs_(item.value.command_id);
    }
    ui::Accelerator key;
    if (actionable && buttons->GetAcceleratorAt(button, &key)) {
      item.accelerator = CaptureAccelerator(key);
      item.text[kAccelerator] = base::UTF16ToUTF8(key.GetShortcutText());
    }
    if (type == ui::ButtonMenuItemModel::TYPE_BUTTON) {
      AddCommand(item, {path, button, item.metadata.source_command_id});
    }
    data_[menu].data.push_back(std::move(item));
  }
  return menu;
}

void MenuModelSnapshot::ItemData::Bind() {
  value.label = Borrow(text[kLabel]);
  value.accelerator = Borrow(text[kAccelerator]);
  value.glyph = Borrow(text[kGlyph]);
  value.metadata = &metadata;
  metadata.secondary_label = Borrow(text[kSecondary]);
  metadata.minor_text = Borrow(text[kMinor]);
  metadata.accessible_name = Borrow(text[kAccessible]);
  metadata.element_identifier = Borrow(text[kElement]);
  metadata.button_row_label = Borrow(text[kButtonRowLabel]);
  metadata.keyboard_accelerator = accelerator ? &*accelerator : nullptr;
  font_views.reserve(fonts.size());
  for (auto& font : fonts) {
    font.value.family = Borrow(font.family);
    font.family_views.reserve(font.actual_families.size());
    for (const auto& family : font.actual_families) {
      font.family_views.push_back(Borrow(family));
    }
    font.value.actual_families = font.family_views.data();
    font.value.actual_family_count = font.family_views.size();
    font_views.push_back(font.value);
  }
  metadata.fonts = font_views.data();
  metadata.font_count = font_views.size();
  metadata.font_size = sizeof(KrMenuFont);
  metadata.colors = colors.data();
  metadata.color_count = colors.size();
  metadata.color_size = sizeof(KrMenuColor);
}

void MenuModelSnapshot::AddCommand(ItemData& item, Command command) {
  CHECK_LT(next_command_, std::numeric_limits<int32_t>::max());
  item.value.command_id = ++next_command_;
  commands_.emplace(next_command_, std::move(command));
}

bool MenuModelSnapshot::KeepsOpen(int32_t command) const {
  for (const auto& menu : data_)
    for (const auto& item : menu.data)
      if (item.value.command_id == command)
        return (item.value.flags & KR_MENU_ITEM_KEEP_OPEN) != 0;
  return false;
}

base::OnceClosure MenuModelSnapshot::CaptureCommand(uint32_t request_id,
                                                    int32_t command,
                                                    int event_flags) {
  if (!valid_ || published_request_ != request_id ||
      !commands_.contains(command)) {
    return {};
  }
  return base::BindOnce(&MenuModelSnapshot::Activate,
                        weak_factory_.GetWeakPtr(), command, event_flags);
}

void MenuModelSnapshot::Activate(int32_t command, int event_flags) {
  const auto found = commands_.find(command);
  if (!valid_ || found == commands_.end()) {
    return;
  }
  // No callbacks are dispatched until the complete source path is validated.
  const auto& action = found->second;
  for (size_t step = 0; step < action.path.size(); ++step) {
    const auto& source = action.path[step];
    if (!source.model || source.index >= source.model->GetItemCount() ||
        source.model->GetCommandIdAt(source.index) != source.command ||
        source.model->GetTypeAt(source.index) != source.type ||
        !source.model->IsEnabledAt(source.index) ||
        !source.model->IsVisibleAt(source.index) ||
        (step + 1 < action.path.size() &&
         source.model->GetSubmenuModelAt(source.index) !=
             action.path[step + 1].model.get())) {
      return;
    }
  }
  const auto& source = action.path.back();
  if (action.button) {
    auto* buttons = source.model->GetButtonMenuItemAt(source.index);
    if (buttons && *action.button < buttons->GetItemCount() &&
        buttons->IsEnabledAt(*action.button) &&
        buttons->GetCommandIdAt(*action.button) == action.button_command &&
        buttons->GetTypeAt(*action.button) ==
            ui::ButtonMenuItemModel::TYPE_BUTTON) {
      buttons->ActivatedAt(*action.button);
    }
  } else {
    source.model->ActivatedAt(source.index, event_flags);
  }
}

void MenuModelSnapshot::Invalidate() {
  if (!valid_ || detaching_) {
    return;
  }
  valid_ = false;
  weak_factory_.InvalidateWeakPtrs();
  if (invalidated_) {
    invalidated_.Run();
  }
}

void MenuModelSnapshot::OnIconChanged(int command_id) {
  // The snapshot owns its images. An asynchronous favicon update does not
  // change command identity; pick up its new pixels on the next capture.
}
void MenuModelSnapshot::OnMenuStructureChanged() {
  if (valid_ && std::getenv("KR_UI_MENU_TRACE"))
    LOG(WARNING) << "menu trace: source structure invalidated snapshot";
  Invalidate();
}
void MenuModelSnapshot::OnMenuClearingDelegate() {
  if (valid_ && std::getenv("KR_UI_MENU_TRACE"))
    LOG(WARNING) << "menu trace: source destruction invalidated snapshot";
  Invalidate();
}

}  // namespace krypton_ui
