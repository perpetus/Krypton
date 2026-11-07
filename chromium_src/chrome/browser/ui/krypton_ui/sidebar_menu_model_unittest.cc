// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/sidebar_menu_model.h"

#include <array>
#include <memory>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "chrome/browser/ui/krypton_ui/menu_model_snapshot.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {
class SidebarMenuTest : public ::testing::Test {
 protected:
  SidebarMenuTest() {
    action_ = actions::ActionItem::Builder(
                  base::BindRepeating(
                      [](int* invoked, actions::ActionItem*,
                         actions::ActionInvocationContext) { ++*invoked; },
                      &invoked_))
                  .SetText(u"Browser action")
                  .Build();
  }
  std::unique_ptr<SidebarMenuModel> Menu() {
    const std::array rows{SidebarMenuModel::Action{42, action_->GetAsWeakPtr(),
                                                   u"Chromium fallback"}};
    return std::make_unique<SidebarMenuModel>(
        rows,
        base::BindRepeating([](SidebarMenuTest* self) { return self->state_; },
                            base::Unretained(this)),
        base::BindRepeating(
            [](SidebarMenuTest* self, int) { return self->command_enabled_; },
            base::Unretained(this)),
        base::BindRepeating(
            [](SidebarMenuTest* self, SidebarMenuModel::Command command) {
              self->dispatched_ = command;
            },
            base::Unretained(this)));
  }
  std::optional<SidebarMenuModel::State> state_ =
      SidebarMenuModel::State{0, true, true};
  bool command_enabled_ = true;
  int invoked_ = 0;
  int dispatched_ = -1;
  std::unique_ptr<actions::ActionItem> action_;
};
TEST_F(SidebarMenuTest, UsesLiveBrowserLabelsAndLiteralAmpersands) {
  auto menu = Menu();
  ASSERT_EQ(menu->GetItemCount(), 8u);
  EXPECT_EQ(menu->GetLabelAt(2), u"Browser action");
  action_->SetText(u"New & changed");
  EXPECT_EQ(menu->GetLabelAt(2), u"New && changed");
  EXPECT_TRUE(menu->IsItemDynamicAt(2));
  action_->SetText(u"");
  EXPECT_EQ(menu->GetLabelAt(2), u"Chromium fallback");
}
TEST_F(SidebarMenuTest, HonorsBrowserPolicyVisibilityAndLifetime) {
  auto menu = Menu();
  menu->ExecuteCommand(42, 0);
  EXPECT_EQ(invoked_, 1);
  command_enabled_ = false;
  menu->ExecuteCommand(42, 0);
  command_enabled_ = true;
  action_->SetEnabled(false);
  menu->ExecuteCommand(42, 0);
  action_->SetEnabled(true);
  action_->SetVisible(false);
  EXPECT_FALSE(menu->IsCommandIdVisible(42));
  menu->ExecuteCommand(42, 0);
  action_.reset();
  EXPECT_FALSE(menu->IsCommandIdEnabled(42));
  EXPECT_FALSE(menu->IsCommandIdVisible(42));
  menu->ExecuteCommand(42, 0);
  EXPECT_EQ(invoked_, 1);
}
TEST_F(SidebarMenuTest,
       ReadsSavedLayoutChecksAndDisablesRightSideHorizontally) {
  auto menu = Menu();
  EXPECT_FALSE(menu->IsCommandIdChecked(SidebarMenuModel::kCompact));
  state_->layout_flags = KR_LAYOUT_COMPACT_MODE | KR_LAYOUT_SIDEBAR_RIGHT;
  EXPECT_TRUE(menu->IsCommandIdChecked(SidebarMenuModel::kCompact));
  EXPECT_TRUE(menu->IsCommandIdChecked(SidebarMenuModel::kSidebarRight));
  EXPECT_TRUE(menu->IsCommandIdEnabled(SidebarMenuModel::kSidebarRight));
  for (uint32_t flag : {KR_LAYOUT_TABS_CLASSIC, KR_LAYOUT_TABS_COMPACT,
                        KR_LAYOUT_TABS_DYNAMIC}) {
    state_->layout_flags = flag;
    EXPECT_FALSE(menu->IsCommandIdEnabled(SidebarMenuModel::kSidebarRight));
    menu->ExecuteCommand(SidebarMenuModel::kSidebarRight, 0);
    EXPECT_EQ(dispatched_, -1);
  }
}
TEST_F(SidebarMenuTest, OptionalNativeEditorsAreGatedByCapabilities) {
  auto menu = Menu();
  state_->can_create_workspace = false;
  state_->can_show_settings = false;
  menu->ExecuteCommand(SidebarMenuModel::kNewWorkspace, 0);
  menu->ExecuteCommand(SidebarMenuModel::kAppearance, 0);
  EXPECT_EQ(dispatched_, -1);
  state_->can_create_workspace = true;
  menu->ExecuteCommand(SidebarMenuModel::kNewWorkspace, 0);
  EXPECT_EQ(dispatched_, SidebarMenuModel::kNewWorkspace);
  state_->can_show_settings = true;
  menu->ExecuteCommand(SidebarMenuModel::kAppearance, 0);
  EXPECT_EQ(dispatched_, SidebarMenuModel::kAppearance);
  menu->ExecuteCommand(SidebarMenuModel::kEditTheme, 0);
  EXPECT_EQ(dispatched_, SidebarMenuModel::kAppearance);
  state_->can_edit_theme = true;
  menu->ExecuteCommand(SidebarMenuModel::kEditTheme, 0);
  EXPECT_EQ(dispatched_, SidebarMenuModel::kEditTheme);
}
TEST_F(SidebarMenuTest, StaleTabOrWindowCannotExecuteAnyCommand) {
  auto menu = Menu();
  state_.reset();
  menu->ExecuteCommand(42, 0);
  menu->ExecuteCommand(SidebarMenuModel::kCompact, 0);
  menu->ExecuteCommand(SidebarMenuModel::kNewWorkspace, 0);
  EXPECT_EQ(invoked_, 0);
  EXPECT_EQ(dispatched_, -1);
  EXPECT_FALSE(menu->IsCommandIdEnabled(42));
  EXPECT_FALSE(menu->IsCommandIdChecked(SidebarMenuModel::kCompact));
}
TEST_F(SidebarMenuTest, UnknownCommandsAndSubmenusDoNotInvokeBrowserActions) {
  auto menu = Menu();
  menu->ExecuteCommand(12345, 0);
  EXPECT_EQ(invoked_, 0);
  EXPECT_EQ(dispatched_, -1);
  EXPECT_FALSE(menu->IsCommandIdEnabled(12345));
}
TEST_F(SidebarMenuTest, DeferredSnapshotRevalidatesBrowserCommandState) {
  auto menu = Menu();
  MenuModelSnapshot snapshot(
      base::BindRepeating(
          [](const ui::ImageModel&) -> base::expected<KrImageId, std::string> {
            return 0;
          }),
      base::DoNothing());
  ASSERT_TRUE(snapshot.Capture(*menu).has_value());
  KrShellUI api{};
  api.struct_size = sizeof(api);
  api.show_menu = [](void*, uint32_t, const KrMenu*, size_t, size_t) {};
  ASSERT_TRUE(snapshot.Publish(api, 1).has_value());
  auto command =
      snapshot.CaptureCommand(1, snapshot.menus()[0].items[2].command_id);
  ASSERT_TRUE(command);
  command_enabled_ = false;
  std::move(command).Run();
  EXPECT_EQ(invoked_, 0);
}
}  // namespace
}  // namespace krypton_ui
