// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#ifndef CHROME_BROWSER_UI_KRYPTON_UI_MENU_MODEL_SNAPSHOT_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_MENU_MODEL_SNAPSHOT_H_

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "chrome/browser/ui/krypton_ui/menu_panel_layout.h"
#include "krypton_ui/abi.h"
#include "ui/base/models/menu_model_delegate.h"

namespace ui {
class ColorProvider;
class ImageModel;
class MenuModel;
}  // namespace ui

namespace krypton_ui {

// Captures the source component's data, rather than a reduced set chosen by the
// current renderer. The menu owner retains its models through dismissal and any
// deferred command, and drives MenuWillShow/MenuWillClose itself. This object
// observes structural changes so a displayed index cannot acquire new meaning.
class MenuModelSnapshot final : public ui::MenuModelDelegate {
 public:
  using PublishImage =
      base::RepeatingCallback<base::expected<KrImageId, std::string>(
          const ui::ImageModel&)>;
  // Returns a static UTF-8 symbol glyph for a source command id, or empty.
  using GlyphTable = std::string_view (*)(int command_id);
  using KeepOpen = bool (*)(int command_id);
  using PanelRow = MenuPanelRow;

  MenuModelSnapshot(
      PublishImage images,
      base::RepeatingClosure invalidated,
      base::RepeatingCallback<void(KrImageId)> release_image = {});
  ~MenuModelSnapshot() override;

  MenuModelSnapshot(const MenuModelSnapshot&) = delete;
  MenuModelSnapshot& operator=(const MenuModelSnapshot&) = delete;

  [[nodiscard]] base::expected<void, std::string> Capture(
      ui::MenuModel& model,
      const ui::ColorProvider* colors = nullptr,
      GlyphTable glyphs = nullptr,
      base::span<const int> root_commands = {},
      KeepOpen keep_open = nullptr);
  [[nodiscard]] bool KeepsOpen(int32_t command) const;
  void Clear();
  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] base::span<const KrMenu> menus() const;
  [[nodiscard]] base::expected<void, std::string> Publish(
      const KrShellUI& ui,
      uint32_t request_id,
      bool panel = false,
      base::span<const PanelRow> layout = {});
  // Take this closure before deferring the browser command. Rebuilding or
  // invalidating the source cancels it; it never looks up a new match by id.
  [[nodiscard]] base::OnceClosure CaptureCommand(uint32_t request_id,
                                                 int32_t command,
                                                 int event_flags = 0);

 private:
  enum TextField {
    kLabel,
    kAccelerator,
    kSecondary,
    kMinor,
    kAccessible,
    kElement,
    kButtonRowLabel,
    kGlyph,
    kTextCount,
  };
  struct FontData {
    KrMenuFont value{};
    std::string family;
    std::vector<std::string> actual_families;
    std::vector<KrStr> family_views;
  };
  struct ItemData {
    KrMenuItem value{};
    KrMenuItemMetadata metadata{};
    std::array<std::string, kTextCount> text;
    std::optional<KrKeyboardAccelerator> accelerator;
    std::vector<FontData> fonts;
    std::vector<KrMenuFont> font_views;
    std::vector<KrMenuColor> colors;
    void Bind();
  };
  struct MenuData {
    std::vector<ItemData> data;
    std::vector<KrMenuItem> items;
  };
  struct SourceItem {
    base::WeakPtr<ui::MenuModel> model;
    size_t index = 0;
    int command = 0;
    int type = 0;
  };
  struct Command {
    std::vector<SourceItem> path;
    std::optional<size_t> button;
    int button_command = 0;
  };

  [[nodiscard]] base::expected<int32_t, std::string> CaptureMenu(
      ui::MenuModel& model,
      std::vector<SourceItem>& path);
  [[nodiscard]] base::expected<ItemData, std::string> CaptureItem(
      ui::MenuModel& model,
      size_t index);
  [[nodiscard]] base::expected<int32_t, std::string> CaptureButtons(
      ui::MenuModel& model,
      size_t index,
      const std::vector<SourceItem>& path);
  [[nodiscard]] base::expected<void, std::string>
  CaptureImages(ui::MenuModel& model, size_t index, ItemData& output);
  void AddCommand(ItemData& item, Command command);
  void Activate(int32_t command, int event_flags);
  void Invalidate();
  void OnIconChanged(int command_id) override;
  void OnMenuStructureChanged() override;
  void OnMenuClearingDelegate() override;

  const PublishImage images_;
  const base::RepeatingCallback<void(KrImageId)> release_image_;
  std::vector<KrImageId> image_leases_;
  // Borrowed only while Capture resolves semantic colors into owned values.
  raw_ptr<const ui::ColorProvider> colors_ = nullptr;
  // Set only while Capture runs.
  GlyphTable glyphs_ = nullptr;
  KeepOpen keep_open_ = nullptr;
  base::span<const int> root_commands_;
  const base::RepeatingClosure invalidated_;
  std::vector<MenuData> data_;
  std::vector<KrMenu> menus_;
  std::vector<base::WeakPtr<ui::MenuModel>> models_;
  base::flat_map<int32_t, Command> commands_;
  int32_t next_command_ = 0;
  bool valid_ = false;
  bool detaching_ = false;
  std::optional<uint32_t> published_request_;
  base::WeakPtrFactory<MenuModelSnapshot> weak_factory_{this};
};

}  // namespace krypton_ui

#endif  // CHROME_BROWSER_UI_KRYPTON_UI_MENU_MODEL_SNAPSHOT_H_
