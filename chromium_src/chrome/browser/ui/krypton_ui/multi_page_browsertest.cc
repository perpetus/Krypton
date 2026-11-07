// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include <array>
#include <string_view>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/run_loop.h"
#include "base/scoped_environment_variable_override.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/renderer_context_menu/render_view_context_menu_test_util.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/multi_page_controller.h"
#include "chrome/browser/ui/krypton_ui/multi_page_features.h"
#include "chrome/browser/ui/krypton_ui/multi_page_menu.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/javascript_dialogs/app_modal_dialog_controller.h"
#include "components/javascript_dialogs/app_modal_dialog_view.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "ui/aura/window.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/test/layer_animator_test_controller.h"
#include "ui/events/event.h"
#include "ui/gfx/scoped_animation_duration_scale_mode.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {
KrTabId Identity(content::WebContents *contents) {
  return base::checked_cast<KrTabId>(
      tabs::TabInterface::GetFromContents(contents)->GetHandle().raw_value());
}

class MultiPageBrowserTest : public InProcessBrowserTest {
public:
  MultiPageBrowserTest() { features_.InitAndEnableFeature(kKryptonMultiPage); }

protected:
  BrowserAdapter *adapter() {
    return BrowserView::GetBrowserViewForBrowser(browser())->krypton_ui();
  }
  KrResult Command(const KrMultiPageCommand &command) {
    const auto result = adapter()->MultiPageCommand(command);
    base::RunLoop().RunUntilIdle();
    return result;
  }
  content::WebContents *Preview(content::WebContents *parent, const GURL &url) {
    NavigateParams params(browser(), url, ui::PAGE_TRANSITION_LINK);
    params.source_contents = parent;
    params.user_gesture = true;
    return adapter()->OpenGlance(params);
  }

private:
  gfx::ScopedAnimationDurationScaleMode animations_{
      gfx::ScopedAnimationDurationScaleMode::ZERO_DURATION};
  base::ScopedEnvironmentVariableOverride experimental_{
      "KR_UI_MULTI_PAGE_EXPERIMENTAL", "1"};
  base::test::ScopedFeatureList features_;
};

views::View *GlanceChrome(BrowserWindowInterface *browser,
                          std::string_view name) {
  auto *pages =
      BrowserView::GetBrowserViewForBrowser(browser)->multi_contents_view();
  pages->GetWidget()->LayoutRootViewIfNecessary();
  for (const auto &child : pages->children())
    if (child->GetClassName() == name)
      return child.get();
  return nullptr;
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       LinkContextMenuGrowsOneThemedGroupToFour) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const auto url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  auto *model = browser()->GetTabStripModel();
  auto *source = model->GetActiveWebContents();
  for (size_t count = 2; count <= 4; ++count) {
    auto menu = TestRenderViewContextMenu::Create(source, url, url);
    menu->Init();
    ASSERT_TRUE(
        menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKSPLITVIEW));
    menu->ExecuteCommand(IDC_CONTENT_CONTEXT_OPENLINKSPLITVIEW, 0);
    base::RunLoop().RunUntilIdle();
    auto *controller = adapter()->multi_page();
    ASSERT_TRUE(controller);
    const auto group = controller->Describe(Identity(source));
    ASSERT_TRUE(group);
    EXPECT_EQ(group->count, count);
    for (int index = 0; index < model->count(); ++index)
      EXPECT_FALSE(model->GetTabAtIndex(index)->IsSplit());
  }
  auto menu = TestRenderViewContextMenu::Create(source, url, url);
  menu->Init();
  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKSPLITVIEW));
  menu->ExecuteCommand(IDC_CONTENT_CONTEXT_OPENLINKSPLITVIEW, 0);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(model->count(), 4);
}
IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       LinkContextSnapSubmenuSelectsRowsAndAsymmetricLayouts) {
  ASSERT_TRUE(adapter()->SnapLayoutsEnabled());
  ASSERT_TRUE(embedded_test_server()->Start());
  const auto url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  auto *model = browser()->GetTabStripModel();
  auto *source = model->GetActiveWebContents();
  for (const auto &[command, layout] :
       {std::pair{MultiPageMenuModel::kRows, KR_SPLIT_ROWS},
        std::pair{MultiPageMenuModel::kMainLeft, KR_SPLIT_MAIN_LEFT},
        std::pair{MultiPageMenuModel::kGrid, KR_SPLIT_GRID}}) {
    auto menu = TestRenderViewContextMenu::Create(source, url, url);
    const auto entry = menu->GetMenuModelAndItemIndex(command);
    ASSERT_TRUE(entry);
    ASSERT_TRUE(entry->first->IsEnabledAt(entry->second));
    entry->first->ActivatedAt(entry->second);
    base::RunLoop().RunUntilIdle();
    const auto group = adapter()->multi_page()->Describe(Identity(source));
    ASSERT_TRUE(group);
    EXPECT_EQ(group->layout, layout);
    EXPECT_EQ(group->count, base::checked_cast<size_t>(model->count()));
  }
  auto full = TestRenderViewContextMenu::Create(source, url, url);
  const auto entry =
      full->GetMenuModelAndItemIndex(MultiPageMenuModel::kMainRight);
  ASSERT_TRUE(entry);
  EXPECT_FALSE(entry->first->IsEnabledAt(entry->second));
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       GlanceHasVerticalControlsAndDismissesFromBackdrop) {
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *model = browser()->GetTabStripModel();
  auto *parent = model->GetActiveWebContents();
  auto *preview =
      Preview(parent, embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  auto preview_weak = preview->GetWeakPtr();
  auto *controls = GlanceChrome(browser(), "GlanceControlsView");
  auto *backdrop = GlanceChrome(browser(), "GlanceBackdrop");
  ASSERT_TRUE(controls && backdrop);
  ASSERT_EQ(controls->children().size(), 3u);
  EXPECT_LT(controls->width(), controls->height());
  EXPECT_LT(controls->children()[0]->y(), controls->children()[1]->y());
  EXPECT_LT(controls->children()[1]->y(), controls->children()[2]->y());
  auto *card = adapter()->multi_page()->ActiveContainer();
  EXPECT_NEAR(card->width(), backdrop->width() * 0.8, 2);
  EXPECT_EQ(card->height(), backdrop->height());
  EXPECT_GE(controls->x(), card->bounds().right());
  const gfx::PointF outside(0, backdrop->height() / 2.0f);
  const ui::MouseEvent click(ui::EventType::kMousePressed, outside, outside,
                             base::TimeTicks::Now(), ui::EF_LEFT_MOUSE_BUTTON,
                             ui::EF_LEFT_MOUSE_BUTTON);
  EXPECT_TRUE(backdrop->OnMousePressed(click));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(preview_weak);
  EXPECT_EQ(model->GetActiveWebContents(), parent);
  EXPECT_FALSE(controls->GetVisible());
  EXPECT_FALSE(backdrop->GetVisible());
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       LinkMenuExtendsSavedNativePairWithoutLosingEitherPage) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const auto url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  auto *model = browser()->GetTabStripModel();
  auto *source = model->GetActiveWebContents();
  chrome::AddTabAt(browser(), url, -1, true);
  auto *companion = model->GetActiveWebContents();
  const auto split = TabListInterface::From(browser())->CreateSplit(
      {tabs::TabInterface::GetFromContents(source)->GetHandle(),
       tabs::TabInterface::GetFromContents(companion)->GetHandle()});
  ASSERT_TRUE(split);
  auto menu = TestRenderViewContextMenu::Create(source, url, url);
  menu->Init();
  menu->ExecuteCommand(IDC_CONTENT_CONTEXT_OPENLINKSPLITVIEW, 0);
  base::RunLoop().RunUntilIdle();
  auto *controller = adapter()->multi_page();
  ASSERT_TRUE(controller);
  const auto group = controller->Describe(Identity(source));
  ASSERT_TRUE(group);
  EXPECT_EQ(group->count, 3u);
  const auto joined = controller->Describe(Identity(companion));
  ASSERT_TRUE(joined);
  EXPECT_EQ(joined->id, group->id);
  EXPECT_EQ(model->count(), 3);
  for (int index = 0; index < model->count(); ++index)
    EXPECT_FALSE(model->GetTabAtIndex(index)->IsSplit());
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       GenieKeepsPageBoundsAndDefersCloseUntilCompletion) {
  gfx::ScopedAnimationDurationScaleMode animations{
      gfx::ScopedAnimationDurationScaleMode::NORMAL_DURATION};
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *preview = Preview(browser()->GetTabStripModel()->GetActiveWebContents(),
                          embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  ASSERT_TRUE(GlanceChrome(browser(), "GlanceControlsView"));
  base::RunLoop().RunUntilIdle();
  auto *card = adapter()->multi_page()->ActiveContainer();
  auto preview_weak = preview->GetWeakPtr();
  int hidden_notifications = 0;
  auto hidden =
      tabs::TabInterface::GetFromContents(preview)->RegisterWillBecomeHidden(
          base::BindRepeating(
              [](int *count, tabs::TabInterface *) { ++*count; },
              &hidden_notifications));
  const auto bounds = card->bounds();
  auto *holder = card->contents_view()->holder();
  const auto native_size = preview->GetNativeView()->bounds().size();
  EXPECT_GE(native_size.width(), holder->width());
  EXPECT_GE(native_size.height(), holder->height());
  // A native child layout during the effect must retain the full renderer
  // viewport instead of capturing the tiny source transform as its size.
  holder->InvalidateLayout();
  card->GetWidget()->LayoutRootViewIfNecessary();
  EXPECT_EQ(preview->GetNativeView()->bounds().size(), native_size);
  ui::LayerAnimatorTestController animation(card->layer()->GetAnimator());
  animation.StartThreadedAnimationsIfNeeded();
  animation.Step(base::Seconds(1));
  EXPECT_EQ(card->bounds(), bounds);
  EXPECT_TRUE(card->GetTransform().IsIdentity());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(card->contents_view()->GetFastResize());
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_CLOSE_GLANCE}),
            KR_OK);
  // Reduced motion bypasses the transition and closes immediately.
  if (preview_weak) {
    EXPECT_EQ(hidden_notifications, 0);
    EXPECT_TRUE(preview->GetPrimaryMainFrame()->IsRenderFrameLive());
    EXPECT_EQ(card->bounds(), bounds);
    animation.StartThreadedAnimationsIfNeeded();
    animation.Step(base::Seconds(1));
    base::RunLoop().RunUntilIdle();
  }
  EXPECT_FALSE(preview_weak);
}

IN_PROC_BROWSER_TEST_F(
    MultiPageBrowserTest,
    RecreatingSplitAfterOrdinaryActivationKeepsOnePageOwner) {
  ASSERT_TRUE(adapter() && adapter()->MultiPageEnabled());
  auto *model = browser()->GetTabStripModel();
  while (model->count() < 3)
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  std::array<content::WebContents *, 3> pages{model->GetWebContentsAt(0),
                                              model->GetWebContentsAt(1),
                                              model->GetWebContentsAt(2)};
  for (size_t attempt = 0; attempt < 6; ++attempt) {
    auto *selected = pages[attempt % pages.size()];
    auto *companion = pages[(attempt + 1) % pages.size()];
    model->ActivateTab(tabs::TabInterface::GetFromContents(selected));
    base::RunLoop().RunUntilIdle();
    ASSERT_TRUE(adapter()->JoinNativeSplit(
        Identity(companion), Identity(selected), KR_SPLIT_COLUMNS));
    base::RunLoop().RunUntilIdle();
    auto *controller = adapter()->multi_page();
    ASSERT_TRUE(controller);
    const auto group = controller->Describe(Identity(selected));
    ASSERT_TRUE(group);
    EXPECT_EQ(group->count, 2u);
    EXPECT_EQ(controller->ActiveContainer()->contents_view()->web_contents(),
              model->GetActiveWebContents());
    // Stock ContentsWebViews each own a Reading Mode overlay. Attaching the
    // same page to a second owner CHECKs even when Reading Mode is closed.
    size_t owners = 0;
    for (const auto &child : BrowserView::GetBrowserViewForBrowser(browser())
                                 ->multi_contents_view()
                                 ->children()) {
      if (auto *container = views::AsViewClass<ContentsContainerView>(child))
        owners += container->contents_view()->web_contents() == selected;
    }
    EXPECT_EQ(owners, 1u);
    ASSERT_TRUE(controller->DetachMember(Identity(companion)));
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(controller->Describe(Identity(selected)));
  }
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       FourPaneEditsPreserveContentsAndTargetFocus) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  auto *model = browser()->GetTabStripModel();
  while (model->count() < 4)
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  std::array<content::WebContents *, 4> contents{};
  std::array<KrSplitMember, 4> members{};
  for (size_t i = 0; i < members.size(); ++i) {
    contents[i] = model->GetWebContentsAt(base::checked_cast<int>(i));
    members[i] = {sizeof(KrSplitMember), Identity(contents[i])};
  }
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_NEW_SPLIT,
                     .tab = members[0].tab,
                     .members = members.data(),
                     .member_count = members.size(),
                     .member_size = sizeof(KrSplitMember),
                     .layout = KR_SPLIT_GRID}),
            KR_OK);
  auto *controller = adapter()->multi_page();
  ASSERT_TRUE(controller);
  int hidden_notifications = 0;
  std::array<base::CallbackListSubscription, 4> visibility;
  std::array<base::CallbackListSubscription, 4> activation;
  for (size_t i = 0; i < contents.size(); ++i)
    visibility[i] = tabs::TabInterface::GetFromContents(contents[i])
                        ->RegisterWillBecomeHidden(base::BindRepeating(
                            [](int *count, tabs::TabInterface *) { ++*count; },
                            &hidden_notifications));
  for (size_t i = 0; i < contents.size(); ++i) {
    activation[i] =
        tabs::TabInterface::GetFromContents(contents[i])
            ->RegisterDidActivate(base::BindRepeating(
                [](MultiPageController *controller, tabs::TabInterface *tab) {
                  // Native RestoreFocus fires from inside the
                  // tab-strip selection mutation. Neither path
                  // may activate the selected tab a second time.
                  controller->Focus(tab->GetContents());
                  controller->ScheduleFocus(tab->GetContents());
                },
                controller));
  }
  for (auto *page : contents) {
    controller->Focus(page);
    EXPECT_EQ(model->GetActiveWebContents(), page);
    EXPECT_EQ(controller->ActiveContainer()->contents_view()->web_contents(),
              page);
  }
  EXPECT_EQ(hidden_notifications, 0);
  const auto group = controller->Describe(members[0].tab);
  ASSERT_TRUE(group);
  KrSplitEdit edit{sizeof(KrSplitEdit), KR_SPLIT_MOVE, group->revision,
                   members[0].tab,      group->space,  3,
                   KR_SPLIT_GRID,       0.5f};
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_EDIT_SPLIT,
                     .group = group->id,
                     .edit = &edit}),
            KR_OK);
  const auto reordered = controller->Describe(members[0].tab);
  ASSERT_TRUE(reordered);
  EXPECT_EQ(reordered->index, 3u);
  edit.kind = KR_SPLIT_DETACH;
  edit.revision = reordered->revision;
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_EDIT_SPLIT,
                     .group = reordered->id,
                     .edit = &edit}),
            KR_OK);
  EXPECT_FALSE(controller->Describe(members[0].tab));
  for (auto *page : contents)
    EXPECT_NE(model->GetIndexOfWebContents(page), TabStripModel::kNoTab);
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       SequentialNativeCommandsGrowToFourAndPreserveIds) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  auto *model = browser()->GetTabStripModel();
  while (model->count() < 5)
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  std::array<content::WebContents *, 5> pages{};
  for (size_t i = 0; i < pages.size(); ++i)
    pages[i] = model->GetWebContentsAt(base::checked_cast<int>(i));
  for (size_t i = 1; i < 4; ++i) {
    ASSERT_TRUE(RedirectNativeSplit(
        tabs::TabInterface::GetFromContents(pages[0]),
        tabs::TabInterface::GetFromContents(pages[i]), false));
    base::RunLoop().RunUntilIdle();
    auto *controller = adapter()->multi_page();
    ASSERT_TRUE(controller);
    const auto group = controller->Describe(Identity(pages[0]));
    ASSERT_TRUE(group);
    EXPECT_EQ(group->count, i + 1);
    for (size_t member = 0; member <= i; ++member) {
      const auto joined = controller->Describe(Identity(pages[member]));
      ASSERT_TRUE(joined);
      EXPECT_EQ(joined->id, group->id);
      EXPECT_EQ(joined->index, member);
      EXPECT_FALSE(
          model->GetSplitForTab(model->GetIndexOfWebContents(pages[member])));
      controller->Focus(pages[member]);
      EXPECT_EQ(controller->ActiveContainer()->contents_view()->web_contents(),
                pages[member]);
    }
  }
  auto *controller = adapter()->multi_page();
  EXPECT_EQ(controller->JoinSplit(Identity(pages[0]), Identity(pages[4]),
                                  KR_SPLIT_COLUMNS),
            KR_ERR_INVALID_ARGUMENT);
  EXPECT_FALSE(controller->Describe(Identity(pages[4])));
  EXPECT_EQ(model->count(), 5);
  // A native focus request from the preceding selection must not override a
  // more recent user command, even when both pages remain in the same group.
  controller->Focus(pages[0]);
  controller->ScheduleFocus(pages[1]);
  controller->Focus(pages[2]);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(model->GetActiveWebContents(), pages[2]);
  controller->ScheduleFocus(pages[1]);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(model->GetActiveWebContents(), pages[1]);
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       NativeSplitApiReturnsRegisteredModelIdentity) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  auto *model = browser()->GetTabStripModel();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  auto *first = model->GetTabAtIndex(0);
  auto *second = model->GetTabAtIndex(1);
  const auto id = TabListInterface::From(browser())->CreateSplit(
      {first->GetHandle(), second->GetHandle()});
  ASSERT_TRUE(id);
  ASSERT_TRUE(model->GetSplitData(*id));
  EXPECT_EQ(model->GetSplitForTab(model->GetIndexOfTab(first)), id);
  EXPECT_EQ(model->GetSplitForTab(model->GetIndexOfTab(second)), id);
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       GlancePromotionKeepsContentsAndNavigationHistory) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *model = browser()->GetTabStripModel();
  auto *parent = model->GetActiveWebContents();
  auto *preview =
      Preview(parent, embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  EXPECT_TRUE(IsTransientGlance(preview));
  EXPECT_EQ(GlanceSessionParent(preview), parent);
  EXPECT_EQ(model->GetActiveWebContents(), preview);
  const auto identity = Identity(preview);
  EXPECT_EQ(Preview(preview, embedded_test_server()->GetURL("/title2.html")),
            preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  EXPECT_TRUE(preview->GetController().CanGoBack());
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_SPLIT_GLANCE}),
            KR_OK);
  EXPECT_EQ(model->GetActiveWebContents(), preview);
  EXPECT_EQ(Identity(preview), identity);
  EXPECT_FALSE(IsTransientGlance(preview));
  EXPECT_EQ(GlanceSessionParent(preview), nullptr);
  EXPECT_TRUE(preview->GetController().CanGoBack());
  const auto group = adapter()->multi_page()->Describe(identity);
  ASSERT_TRUE(group);
  EXPECT_EQ(group->count, 2u);
  EXPECT_TRUE(adapter()->multi_page()->Describe(Identity(parent)));
}

IN_PROC_BROWSER_TEST_F(
    MultiPageBrowserTest,
    GlanceOverThreePanesPromotesIntoFourthWithoutRecreation) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *model = browser()->GetTabStripModel();
  while (model->count() < 3)
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  std::array<KrSplitMember, 3> members{};
  for (size_t i = 0; i < members.size(); ++i)
    members[i] = {sizeof(KrSplitMember), Identity(model->GetWebContentsAt(
                                             base::checked_cast<int>(i)))};
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_NEW_SPLIT,
                     .tab = members[0].tab,
                     .members = members.data(),
                     .member_count = members.size(),
                     .member_size = sizeof(KrSplitMember),
                     .layout = KR_SPLIT_GRID}),
            KR_OK);
  auto *parent = model->GetActiveWebContents();
  auto *preview =
      Preview(parent, embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  auto *controller = adapter()->multi_page();
  EXPECT_EQ(model->GetActiveWebContents(), preview);
  EXPECT_EQ(controller->ActiveContainer()->contents_view()->web_contents(),
            preview);
  // This synchronous notification reproduces RestoreFocus inside a selection
  // callback; it must acknowledge the current child without another activation.
  controller->Focus(preview);
  controller->ScheduleFocus(preview);
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_SPLIT_GLANCE}),
            KR_OK);
  const auto group = controller->Describe(Identity(preview));
  ASSERT_TRUE(group);
  EXPECT_EQ(group->count, 4u);
  EXPECT_EQ(model->GetActiveWebContents(), preview);
  EXPECT_EQ(controller->ActiveContainer()->contents_view()->web_contents(),
            preview);
  EXPECT_FALSE(IsTransientGlance(preview));
  for (const auto member : members) {
    const auto preserved = controller->Describe(member.tab);
    ASSERT_TRUE(preserved);
    EXPECT_EQ(preserved->id, group->id);
  }
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       SplittingGlanceCopiesPinnedGroupAndKeepsChildIdentity) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *model = browser()->GetTabStripModel();
  while (model->count() < 2)
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  std::array<content::WebContents *, 2> originals{model->GetWebContentsAt(0),
                                                  model->GetWebContentsAt(1)};
  std::array<KrSplitMember, 2> members{};
  for (size_t i = 0; i < originals.size(); ++i) {
    model->SetTabPinned(model->GetIndexOfWebContents(originals[i]), true);
    members[i] = {sizeof(KrSplitMember), Identity(originals[i])};
  }
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_NEW_SPLIT,
                     .tab = members[0].tab,
                     .members = members.data(),
                     .member_count = members.size(),
                     .member_size = sizeof(KrSplitMember),
                     .layout = KR_SPLIT_COLUMNS}),
            KR_OK);
  const auto saved_group = adapter()->multi_page()->Describe(members[0].tab);
  ASSERT_TRUE(saved_group);
  auto *preview =
      Preview(originals[0], embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  const auto child_id = Identity(preview);
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_SPLIT_GLANCE}),
            KR_OK);
  EXPECT_EQ(Identity(preview), child_id);
  EXPECT_EQ(model->GetActiveWebContents(), preview);
  const auto new_group = adapter()->multi_page()->Describe(child_id);
  ASSERT_TRUE(new_group);
  EXPECT_NE(new_group->id, saved_group->id);
  EXPECT_EQ(new_group->count, 3u);
  EXPECT_FALSE(model->IsTabPinned(model->GetIndexOfWebContents(preview)));
  for (auto *original : originals) {
    EXPECT_TRUE(model->IsTabPinned(model->GetIndexOfWebContents(original)));
    const auto kept = adapter()->multi_page()->Describe(Identity(original));
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->id, saved_group->id);
    EXPECT_EQ(kept->count, 2u);
  }
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       ClosingGlanceReleasesChildWithoutRecentlyClosedEntry) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *model = browser()->GetTabStripModel();
  auto *parent = model->GetActiveWebContents();
  auto *restore =
      TabRestoreServiceFactory::GetForProfile(browser()->GetProfile());
  ASSERT_TRUE(restore);
  const auto recently_closed = restore->entries().size();
  const auto tab_count = model->count();
  auto *preview =
      Preview(parent, embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  content::WebContentsDestroyedWatcher destroyed(preview);
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_CLOSE_GLANCE}),
            KR_OK);
  destroyed.Wait();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(model->count(), tab_count);
  EXPECT_EQ(model->GetActiveWebContents(), parent);
  EXPECT_EQ(restore->entries().size(), recently_closed);
}

IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       CancellingGlanceUnloadCancelsParentDeparture) {
  ASSERT_TRUE(adapter());
  ASSERT_TRUE(adapter()->MultiPageEnabled());
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *model = browser()->GetTabStripModel();
  auto *parent = model->GetActiveWebContents();
  auto *preview =
      Preview(parent, embedded_test_server()->GetURL("/beforeunload.html"));
  ASSERT_TRUE(preview);
  ASSERT_TRUE(content::WaitForLoadStop(preview));
  content::PrepContentsForBeforeUnloadTest(preview);
  model->ActivateTab(tabs::TabInterface::GetFromContents(parent));
  auto *dialog = ui_test_utils::WaitForAppModalDialog();
  ASSERT_TRUE(dialog);
  dialog->view()->CancelAppModalDialog();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(model->GetActiveWebContents(), preview);
  EXPECT_TRUE(IsTransientGlance(preview));
  EXPECT_NE(model->GetIndexOfWebContents(parent), TabStripModel::kNoTab);
  // Successful promotion also proves that cancellation restored the open
  // phase, rather than leaving the controller waiting for a removed child.
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_EXPAND_GLANCE}),
            KR_OK);
  EXPECT_FALSE(IsTransientGlance(preview));
  EXPECT_EQ(model->GetActiveWebContents(), preview);
  ASSERT_TRUE(content::ExecJs(preview, "window.onbeforeunload = null"));
}
IN_PROC_BROWSER_TEST_F(
    MultiPageBrowserTest,
    SnapContextSubmenuPreservesLivePagesAndRejectsStaleMenu) {
  ASSERT_TRUE(adapter()->SnapLayoutsEnabled());
  auto *model = browser()->GetTabStripModel();
  while (model->count() < 4)
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  std::array<KrSplitMember, 4> members{};
  std::array<content::WebContents *, 4> pages{};
  for (size_t i = 0; i < pages.size(); ++i) {
    pages[i] = model->GetWebContentsAt(base::checked_cast<int>(i));
    members[i] = {sizeof(KrSplitMember), Identity(pages[i])};
  }
  auto *tab = tabs::TabInterface::GetFromContents(pages[0]);
  MultiPageMenuModel ordinary(*browser(), *tab);
  EXPECT_TRUE(ordinary.IsCommandIdEnabled(MultiPageMenuModel::kMainLeft));
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_NEW_SPLIT,
                     .tab = members[0].tab,
                     .members = members.data(),
                     .member_count = members.size(),
                     .member_size = sizeof(KrSplitMember),
                     .layout = KR_SPLIT_GRID}),
            KR_OK);
  for (const auto [command, layout] :
       {std::pair{MultiPageMenuModel::kRows, KR_SPLIT_ROWS},
        std::pair{MultiPageMenuModel::kWideLeft, KR_SPLIT_WIDE_LEFT},
        std::pair{MultiPageMenuModel::kMainLeft, KR_SPLIT_MAIN_LEFT},
        std::pair{MultiPageMenuModel::kMainRight, KR_SPLIT_MAIN_RIGHT},
        std::pair{MultiPageMenuModel::kMainTop, KR_SPLIT_MAIN_TOP},
        std::pair{MultiPageMenuModel::kMainBottom, KR_SPLIT_MAIN_BOTTOM},
        std::pair{MultiPageMenuModel::kGrid, KR_SPLIT_GRID}}) {
    MultiPageMenuModel menu(*browser(), *tab);
    ASSERT_TRUE(menu.IsCommandIdEnabled(command));
    menu.ExecuteCommand(command, 0);
    base::RunLoop().RunUntilIdle();
    const auto group = adapter()->multi_page()->Describe(members[0].tab);
    ASSERT_TRUE(group);
    EXPECT_EQ(group->layout, layout);
    EXPECT_EQ(group->count, 4u);
    EXPECT_FALSE(menu.IsCommandIdEnabled(command));
    for (auto *page : pages)
      EXPECT_NE(model->GetIndexOfWebContents(page), TabStripModel::kNoTab);
  }
}
IN_PROC_BROWSER_TEST_F(MultiPageBrowserTest,
                       RepeatedGlanceCloseReconcilesBeforeSplitSelection) {
  ASSERT_TRUE(embedded_test_server()->Start());
  auto *model = browser()->GetTabStripModel();
  while (model->count() < 4)
    chrome::AddTabAt(browser(), GURL("about:blank"), -1, true);
  std::array<KrSplitMember, 4> members{};
  for (size_t i = 0; i < members.size(); ++i)
    members[i] = {sizeof(KrSplitMember), Identity(model->GetWebContentsAt(
                                             base::checked_cast<int>(i)))};
  ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                     .kind = KR_MULTI_PAGE_NEW_SPLIT,
                     .tab = members[0].tab,
                     .members = members.data(),
                     .member_count = members.size(),
                     .member_size = sizeof(KrSplitMember),
                     .layout = KR_SPLIT_MAIN_RIGHT}),
            KR_OK);
  for (size_t cycle = 0; cycle < 8; ++cycle) {
    auto *parent = model->GetWebContentsAt(base::checked_cast<int>(cycle % 4));
    adapter()->multi_page()->Focus(parent);
    auto *preview =
        Preview(parent, embedded_test_server()->GetURL("/title1.html"));
    ASSERT_TRUE(preview);
    ASSERT_TRUE(content::WaitForLoadStop(preview));
    auto weak = preview->GetWeakPtr();
    ASSERT_EQ(Command({.struct_size = sizeof(KrMultiPageCommand),
                       .kind = KR_MULTI_PAGE_CLOSE_GLANCE}),
              KR_OK);
    EXPECT_FALSE(weak);
    EXPECT_EQ(model->count(), 4);
    EXPECT_EQ(model->GetActiveWebContents(), parent);
    EXPECT_EQ(adapter()
                  ->multi_page()
                  ->ActiveContainer()
                  ->contents_view()
                  ->web_contents(),
              parent);
    const auto group = adapter()->multi_page()->Describe(Identity(parent));
    ASSERT_TRUE(group);
    EXPECT_EQ(group->count, 4u);
    EXPECT_EQ(group->layout, KR_SPLIT_MAIN_RIGHT);
  }
}
} // namespace
} // namespace krypton_ui
