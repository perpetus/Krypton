// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/site_data_menu_model.h"

#include <array>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace krypton_ui {
namespace {
class SiteDataMenuTest : public testing::Test {
 protected:
  std::unique_ptr<SiteDataMenuModel> Menu() {
    const std::array actions{SiteDataMenuModel::Action{
        42, action_->GetAsWeakPtr(), u"Browser fallback"}};
    return std::make_unique<SiteDataMenuModel>(
        actions, std::make_unique<ui::SimpleMenuModel>(nullptr),
        std::make_unique<ui::SimpleMenuModel>(nullptr),
        SiteDataMenuModel::Labels{u"Extensions", u"Settings", u"Cookies",
                                  u"Site settings", u"More"},
        base::BindRepeating([](SiteDataMenuTest* self) { return self->state_; },
                            base::Unretained(this)),
        base::BindRepeating(
            [](SiteDataMenuTest* self, int) { return self->allowed_; },
            base::Unretained(this)),
        base::BindRepeating(
            [](SiteDataMenuTest* self, SiteDataMenuModel::Command command) {
              self->dispatched_ = command;
            },
            base::Unretained(this)));
  }
  int invoked_ = 0, dispatched_ = -1;
  bool allowed_ = true;
  std::optional<SiteDataMenuModel::State> state_ =
      SiteDataMenuModel::State{true, true, u"Secure", u"Blocked for this site"};
  std::unique_ptr<actions::ActionItem> action_ =
      actions::ActionItem::Builder(
          base::BindRepeating(
              [](int* invoked,
                 actions::ActionItem*,
                 actions::ActionInvocationContext) { ++*invoked; },
              &invoked_))
          .SetText(u"Real browser action")
          .Build();
};
TEST_F(SiteDataMenuTest, UsesLiveActionsAndBrowserEnablement) {
  auto menu = Menu();
  auto* page = menu->GetSubmenuModelAt(0);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->GetLabelAt(0), u"Real browser action");
  action_->SetText(u"Updated & title");
  EXPECT_EQ(page->GetLabelAt(0), u"Updated && title");
  page->ActivatedAt(0);
  EXPECT_EQ(invoked_, 1);
  allowed_ = false;
  page->ActivatedAt(0);
  action_.reset();
  EXPECT_FALSE(page->IsEnabledAt(0));
  EXPECT_FALSE(page->IsVisibleAt(0));
  EXPECT_EQ(invoked_, 1);
}
TEST_F(SiteDataMenuTest, ReflectsActualSiteStatusAndRejectsStaleTargets) {
  auto menu = Menu();
  EXPECT_EQ(menu->GetSecondaryLabelAt(4), u"Blocked for this site");
  auto* footer = menu->GetSubmenuModelAt(5);
  ASSERT_NE(footer, nullptr);
  EXPECT_EQ(footer->GetLabelAt(0), u"Secure");
  footer->ActivatedAt(0);
  EXPECT_EQ(dispatched_, SiteDataMenuModel::kSecureConnection);
  state_->site_available = false;
  EXPECT_FALSE(menu->IsVisibleAt(4));
  EXPECT_FALSE(footer->IsEnabledAt(0));
  state_.reset();
  dispatched_ = -1;
  menu->ExecuteCommand(42, 0);
  footer->ActivatedAt(1);
  EXPECT_EQ(invoked_, 0);
  EXPECT_EQ(dispatched_, -1);
}
}  // namespace
}  // namespace krypton_ui
