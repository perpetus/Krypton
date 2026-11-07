// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/menu_model_snapshot.h"

#include <array>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/models/image_model.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
namespace {

class MenuSnapshotTest : public ::testing::Test,
                         public ui::SimpleMenuModel::Delegate {
 public:
  MenuSnapshotTest()
      : snapshot_(
            base::BindRepeating(
                [](const ui::ImageModel&)
                    -> base::expected<KrImageId, std::string> { return 0; }),
            base::BindRepeating([](int* count) { ++*count; },
                                &invalidations_)) {}

  bool IsCommandIdEnabled(int command) const override { return enabled_; }
  bool IsCommandIdVisible(int command) const override { return visible_; }
  void ExecuteCommand(int command, int flags) override {
    executed_ = command;
    flags_ = flags;
  }
  bool Publish(uint32_t request = 7) {
    KrShellUI api{};
    api.struct_size = sizeof(api);
    api.show_menu = [](void*, uint32_t, const KrMenu*, size_t, size_t) {};
    return snapshot_.Publish(api, request).has_value();
  }
  int Command(size_t menu = 0, size_t item = 0) const {
    return snapshot_.menus()[menu].items[item].command_id;
  }

 protected:
  bool enabled_ = true;
  bool visible_ = true;
  int executed_ = -1;
  int flags_ = 0;
  int invalidations_ = 0;
  MenuModelSnapshot snapshot_;
};

TEST_F(MenuSnapshotTest, OwnsMetadataAndRevalidatesDeferredCommand) {
  ui::SimpleMenuModel model(this);
  model.AddItem(42, u"Original label");
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  const auto& item = snapshot_.menus()[0].items[0];
  EXPECT_EQ(std::string_view(item.label.data, item.label.size),
            "Original label");
  ASSERT_NE(item.metadata, nullptr);
  EXPECT_EQ(item.metadata->source_command_id, 42);
  auto command = snapshot_.CaptureCommand(7, Command(), 123);
  ASSERT_TRUE(command);
  EXPECT_EQ(executed_, -1);
  enabled_ = false;
  std::move(command).Run();
  EXPECT_EQ(executed_, -1);
  enabled_ = true;
  auto valid = snapshot_.CaptureCommand(7, Command(), 123);
  std::move(valid).Run();
  EXPECT_EQ(executed_, 42);
  EXPECT_EQ(flags_, 123);
}

TEST_F(MenuSnapshotTest, RepublishAfterRowEditsKeepsCommandIds) {
  ui::SimpleMenuModel model(this);
  model.AddItem(41, u"Copy");
  model.AddItem(42, u"Copy link");
  model.SetEnabledAt(1, false);
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  const int before = Command(0, 1);
  EXPECT_FALSE(snapshot_.menus()[0].items[1].flags & KR_MENU_ITEM_ENABLED);
  // A page menu enables its row once the link to the selection is ready.
  model.SetLabel(1, u"Copy link to highlight");
  model.SetEnabledAt(1, true);
  EXPECT_EQ(invalidations_, 0);
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  const auto& item = snapshot_.menus()[0].items[1];
  EXPECT_EQ(item.command_id, before);
  EXPECT_TRUE(item.flags & KR_MENU_ITEM_ENABLED);
  EXPECT_EQ(std::string_view(item.label.data, item.label.size),
            "Copy link to highlight");
  auto command = snapshot_.CaptureCommand(7, before);
  ASSERT_TRUE(command);
  std::move(command).Run();
  EXPECT_EQ(executed_, 42);
}

TEST_F(MenuSnapshotTest, TiledPanelKeepsCommandsAndFallsBackToOrdinaryRows) {
  ui::SimpleMenuModel root(this), child(this);
  child.AddItem(77, u"Real command");
  root.AddSubMenu(42, u"Page actions", &child);
  ASSERT_TRUE(snapshot_.Capture(root).has_value());
  const std::array<MenuModelSnapshot::PanelRow, 1> layout{
      {{42, KR_MENU_ITEM_GRID}}};
  uint32_t received = 0;
  KrShellUI api{};
  api.struct_size = sizeof(api);
  api.self = &received;
  api.show_menu = [](void*, uint32_t, const KrMenu*, size_t, size_t) {};
  api.show_menu_panel_with_tiles = [](void* self, uint32_t, const KrMenu* menus,
                                      size_t, size_t) {
    *static_cast<uint32_t*>(self) = menus[0].items[0].type;
  };
  api.show_menu_panel = api.show_menu_panel_with_tiles;
  ASSERT_TRUE(snapshot_.Publish(api, 7, true, layout).has_value());
  EXPECT_EQ(received, uint32_t{KR_MENU_ITEM_GRID});
  EXPECT_EQ(snapshot_.menus()[0].items[0].metadata->source_command_id, 42);
  auto execute = snapshot_.CaptureCommand(7, Command(1));
  ASSERT_TRUE(execute);
  std::move(execute).Run();
  EXPECT_EQ(executed_, 77);
  api.struct_size = offsetof(KrShellUI, show_menu_panel_with_tiles);
  ASSERT_TRUE(snapshot_.Publish(api, 7, true, layout).has_value());
  EXPECT_EQ(received, uint32_t{KR_MENU_ITEM_SUBMENU});
  api.show_menu = api.show_menu_panel;
  api.struct_size = offsetof(KrShellUI, show_menu_panel);
  ASSERT_TRUE(snapshot_.Publish(api, 7, true, layout).has_value());
  EXPECT_EQ(received, uint32_t{KR_MENU_ITEM_SUBMENU});
}

TEST_F(MenuSnapshotTest, CapturesGlyphsUsingSourceCommandsAndKeepsMnemonics) {
  ui::SimpleMenuModel model(this);
  model.AddItem(42, u"&New tab");
  model.AddItem(43, u"Other");
  ASSERT_TRUE(snapshot_
                  .Capture(model, nullptr,
                           [](int command) -> std::string_view {
                             return command == 42 ? "\xEE\xA4\xA5" : "";
                           })
                  .has_value());
  ASSERT_TRUE(Publish());
  const auto& item = snapshot_.menus()[0].items[0];
  EXPECT_EQ(std::string_view(item.glyph.data, item.glyph.size), "\xEE\xA4\xA5");
  EXPECT_EQ(std::string_view(item.label.data, item.label.size), "&New tab");
  ASSERT_NE(item.metadata, nullptr);
  EXPECT_NE(item.metadata->flags & KR_MENU_META_MAY_HAVE_MNEMONICS, 0u);
  EXPECT_EQ(snapshot_.menus()[0].items[1].glyph.size, 0u);
}

TEST_F(MenuSnapshotTest, StructuralChangeCancelsCapturedCommand) {
  ui::SimpleMenuModel model(this);
  model.AddItem(42, u"First");
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  auto command = snapshot_.CaptureCommand(7, Command());
  model.RemoveItemAt(0);
  model.AddItem(42, u"Replacement using the same command id");
  EXPECT_FALSE(snapshot_.valid());
  EXPECT_EQ(invalidations_, 1);
  std::move(command).Run();
  EXPECT_EQ(executed_, -1);
}

TEST_F(MenuSnapshotTest, RecaptureAfterStructuralChangeFollowsNewRows) {
  // Windows spelling removes unused suggestion placeholders after the menu
  // opens. Its open request is captured again rather than dismissed.
  ui::SimpleMenuModel model(this);
  model.AddItem(41, u"Suggestion");
  model.AddItem(42, u"Unused placeholder");
  model.AddItem(44, u"Another unused placeholder");
  model.AddItem(43, u"Add to dictionary");
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  auto stale = snapshot_.CaptureCommand(7, Command(0, 3));
  model.RemoveItemAt(1);
  model.RemoveItemAt(1);
  EXPECT_EQ(invalidations_, 1);
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  std::move(stale).Run();
  EXPECT_EQ(executed_, -1);
  ASSERT_EQ(snapshot_.menus()[0].count, 2u);
  auto current = snapshot_.CaptureCommand(7, Command(0, 1));
  ASSERT_TRUE(current);
  std::move(current).Run();
  EXPECT_EQ(executed_, 43);
}

TEST_F(MenuSnapshotTest, RebuiltSubmenuCannotRedirectAnOldCommand) {
  auto old_menu = std::make_unique<ui::SimpleMenuModel>(this);
  old_menu->AddItem(41, u"Old history entry");
  ui::SimpleMenuModel root(this);
  root.AddSubMenu(40, u"History", old_menu.get());
  ASSERT_TRUE(snapshot_.Capture(root).has_value());
  ASSERT_TRUE(Publish());
  auto stale = snapshot_.CaptureCommand(7, Command(1));
  root.RemoveItemAt(0);
  old_menu.reset();
  ui::SimpleMenuModel replacement(this);
  replacement.AddItem(42, u"New history entry");
  root.AddSubMenu(40, u"History", &replacement);
  EXPECT_EQ(invalidations_, 1);
  ASSERT_TRUE(snapshot_.Capture(root).has_value());
  ASSERT_TRUE(Publish());
  std::move(stale).Run();
  EXPECT_EQ(executed_, -1);
  auto current = snapshot_.CaptureCommand(7, Command(1));
  ASSERT_TRUE(current);
  std::move(current).Run();
  EXPECT_EQ(executed_, 42);
}

TEST_F(MenuSnapshotTest, AsyncFaviconDoesNotDismissOrCancelTheMenu) {
  ui::SimpleMenuModel model(this);
  model.AddItem(42, u"Source tab");
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  auto command = snapshot_.CaptureCommand(7, Command());
  ASSERT_TRUE(command);
  // RecentTabsSubMenuModel updates its item, then notifies its presenter.
  model.SetIcon(0, ui::ImageModel());
  model.menu_model_delegate()->OnIconChanged(42);
  EXPECT_TRUE(snapshot_.valid());
  EXPECT_EQ(invalidations_, 0);
  std::move(command).Run();
  EXPECT_EQ(executed_, 42);
}

TEST_F(MenuSnapshotTest, ProjectionKeepsSourceRowsAndCommandIndices) {
  ui::SimpleMenuModel child(this);
  child.AddItem(72, u"Child supplied by Chromium");
  ui::SimpleMenuModel model(this);
  model.AddItem(40, u"Excluded");
  model.AddItem(42, u"Real source label");
  model.AddSubMenu(70, u"Source submenu", &child);
  constexpr std::array commands{42, 70, 99};
  ASSERT_TRUE(snapshot_.Capture(model, nullptr, nullptr, commands).has_value());
  ASSERT_TRUE(Publish());
  ASSERT_EQ(snapshot_.menus()[0].count, 2u);
  const auto& first = snapshot_.menus()[0].items[0];
  EXPECT_EQ(std::string_view(first.label.data, first.label.size),
            "Real source label");
  EXPECT_EQ(first.metadata->source_command_id, 42);
  auto execute = snapshot_.CaptureCommand(7, Command());
  ASSERT_TRUE(execute);
  std::move(execute).Run();
  EXPECT_EQ(executed_, 42);
  ASSERT_EQ(snapshot_.menus()[1].count, 1u);
  auto nested = snapshot_.CaptureCommand(7, Command(1));
  ASSERT_TRUE(nested);
  std::move(nested).Run();
  EXPECT_EQ(executed_, 72);
}

TEST_F(MenuSnapshotTest, DestroyedSourceCancelsDeferredExecution) {
  auto model = std::make_unique<ui::SimpleMenuModel>(this);
  model->AddItem(42, u"First");
  ASSERT_TRUE(snapshot_.Capture(*model).has_value());
  ASSERT_TRUE(Publish());
  auto command = snapshot_.CaptureCommand(7, Command());
  model.reset();
  std::move(command).Run();
  EXPECT_EQ(executed_, -1);
  snapshot_.Clear();
}

TEST_F(MenuSnapshotTest, RechecksDisabledAncestorAndVisibility) {
  ui::SimpleMenuModel child(this);
  child.AddItem(42, u"Child");
  ui::SimpleMenuModel root(this);
  root.AddSubMenu(99, u"Parent", &child);
  ASSERT_TRUE(snapshot_.Capture(root).has_value());
  ASSERT_TRUE(Publish());
  auto command = snapshot_.CaptureCommand(7, Command(1));
  visible_ = false;
  std::move(command).Run();
  EXPECT_EQ(executed_, -1);
}

TEST_F(MenuSnapshotTest, RepublishAndClearInvalidateQueuedCommands) {
  ui::SimpleMenuModel model(this);
  model.AddItem(42, u"First");
  ASSERT_TRUE(snapshot_.Capture(model).has_value());
  ASSERT_TRUE(Publish());
  auto old = snapshot_.CaptureCommand(7, Command());
  ASSERT_TRUE(Publish(8));
  EXPECT_FALSE(snapshot_.CaptureCommand(7, Command()));
  std::move(old).Run();
  EXPECT_EQ(executed_, -1);
  auto current = snapshot_.CaptureCommand(8, Command());
  snapshot_.Clear();
  std::move(current).Run();
  EXPECT_EQ(executed_, -1);
  EXPECT_EQ(model.menu_model_delegate(), nullptr);
}

}  // namespace
}  // namespace krypton_ui
