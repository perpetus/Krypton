// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/action_menu_model.h"

#include <memory>

#include "base/functional/bind.h"
#include "chrome/browser/ui/krypton_ui/extension_actions.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
namespace {
class ExtensionActionMenuTest : public testing::Test,
                                public ui::SimpleMenuModel::Delegate {
 public:
  ExtensionActionMenuTest() {
    source = std::make_unique<ui::SimpleMenuModel>(this);
    source->AddCheckItem(77, u"Pin to toolbar");
    source->AddItem(91, u"Real extension options");
  }
  std::unique_ptr<ActionMenuModel> Menu(bool action = true) {
    if (!action)
      return std::make_unique<ActionMenuModel>(*source);
    return std::make_unique<ActionMenuModel>(
        *source,
        ActionMenuModel::Action{
            u"Real & extension",
            {},
            base::BindRepeating([](bool* enabled) { return *enabled; },
                                &invoke_enabled),
            base::BindRepeating([](int* calls) { ++*calls; }, &invocations)});
  }
  bool IsCommandIdChecked(int command) const override { return pinned; }
  bool IsCommandIdEnabled(int command) const override { return source_enabled; }
  bool IsCommandIdVisible(int command) const override { return source_visible; }
  void ExecuteCommand(int command, int flags) override { executed = command; }
  std::unique_ptr<ui::SimpleMenuModel> source;
  bool pinned = false;
  bool source_enabled = true;
  bool source_visible = true;
  bool invoke_enabled = true;
  int executed = 0;
  int invocations = 0;
};
TEST_F(ExtensionActionMenuTest,
       RetainsSourceCommandIdentityAfterLeadingAction) {
  auto menu = Menu();
  ASSERT_EQ(menu->GetItemCount(), 4u);
  EXPECT_EQ(menu->GetTypeAt(1), ui::MenuModel::TYPE_SEPARATOR);
  EXPECT_EQ(menu->GetCommandIdAt(2), 77);
  EXPECT_EQ(menu->GetLabelAt(2), u"Pin to toolbar");
  menu->ActivatedAt(2, 0);
  EXPECT_EQ(executed, 77);
  EXPECT_EQ(invocations, 0);
}
TEST_F(ExtensionActionMenuTest,
       InvokesActualActionSeparatelyFromContextCommands) {
  auto menu = Menu();
  menu->ActivatedAt(0, 0);
  EXPECT_EQ(invocations, 1);
  EXPECT_EQ(executed, 0);
  EXPECT_EQ(menu->GetLabelAt(0), u"Real & extension");
  EXPECT_FALSE(menu->MayHaveMnemonicsAt(0));
}
TEST_F(ExtensionActionMenuTest, RechecksActionEnablementAtExecution) {
  auto menu = Menu();
  invoke_enabled = false;
  menu->ActivatedAt(0, 0);
  EXPECT_FALSE(menu->IsEnabledAt(0));
  EXPECT_EQ(invocations, 0);
}
TEST_F(ExtensionActionMenuTest, ReadsCurrentPinStateAndLabelFromSource) {
  auto menu = Menu();
  EXPECT_FALSE(menu->IsItemCheckedAt(2));
  pinned = true;
  source->SetLabel(0, u"Unpin from toolbar");
  EXPECT_TRUE(menu->IsItemCheckedAt(2));
  EXPECT_EQ(menu->GetLabelAt(2), u"Unpin from toolbar");
}
TEST_F(ExtensionActionMenuTest, RechecksSourcePolicyAndVisibility) {
  auto menu = Menu();
  source_enabled = false;
  menu->ActivatedAt(2, 0);
  EXPECT_FALSE(menu->IsEnabledAt(2));
  EXPECT_EQ(executed, 0);
  source_enabled = true;
  source_visible = false;
  menu->ActivatedAt(2, 0);
  EXPECT_FALSE(menu->IsVisibleAt(2));
  EXPECT_EQ(executed, 0);
}
TEST_F(ExtensionActionMenuTest, UnloadedExtensionCannotExecuteQueuedCommands) {
  auto menu = Menu();
  source.reset();
  EXPECT_EQ(menu->GetItemCount(), 0u);
  menu->ActivatedAt(0, 0);
  menu->ActivatedAt(2, 0);
  EXPECT_EQ(invocations, 0);
  EXPECT_EQ(executed, 0);
}
TEST_F(ExtensionActionMenuTest, ToolbarContextMenuHasNoSyntheticLeadingRows) {
  auto menu = Menu(false);
  ASSERT_EQ(menu->GetItemCount(), 2u);
  EXPECT_EQ(menu->GetCommandIdAt(0), 77);
  EXPECT_EQ(menu->GetTypeAt(0), ui::MenuModel::TYPE_CHECK);
  menu->ActivatedAt(1, 0);
  EXPECT_EQ(executed, 91);
}
TEST(ExtensionActionTest, MenuButtonLivesAtRightOfAddressField) {
  constexpr char name[] = "Chromium extensions";
  const auto action = ExtensionMenuAction({name, sizeof(name) - 1});
  EXPECT_EQ(action.kind, KR_ACTION_SITE_SETTING);
  EXPECT_EQ(action.id, kExtensionMenuAction);
  EXPECT_EQ(action.flags,
            static_cast<uint32_t>(KR_ACTION_VISIBLE | KR_ACTION_ENABLED));
  EXPECT_GT(action.glyph.size, 0u);
  EXPECT_TRUE(IsExtensionAction(action.id));
  EXPECT_TRUE(IsExtensionAction((uint64_t{4} << 56) | 1));
  EXPECT_FALSE(IsExtensionAction((uint64_t{1} << 56) | 1));
  EXPECT_FALSE(IsExtensionAction((uint64_t{2} << 56) | 1));
}
}  // namespace
}  // namespace krypton_ui
