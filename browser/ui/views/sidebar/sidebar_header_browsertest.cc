// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_header.h"

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/function_ref.h"
#include "base/threading/thread_restrictions.h"
#include "base/test/run_until.h"
#include "chrome/browser/extensions/chrome_test_extension_loader.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/animation/browser_animation_controller.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_element_identifiers.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/animations/tab_strip_animations.h"
#include "chrome/browser/ui/views/bubble_anchor_util_views.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_bubble_delegate_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/page_info/page_info_bubble_view_base.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_combo_button.h"
#include "chrome/browser/ui/views/toolbar/back_forward_button.h"
#include "chrome/browser/ui/views/toolbar/toolbar_action_view.h"
#include "chrome/browser/ui/views/toolbar/reload_button.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "extensions/common/extension.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_test_api.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/test/button_test_api.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace strata {
namespace {

template <typename T>
T* FindDescendant(views::View* root) {
  if (auto* match = views::AsViewClass<T>(root)) {
    return match;
  }
  for (views::View* child : root->children()) {
    if (T* match = FindDescendant<T>(child)) {
      return match;
    }
  }
  return nullptr;
}

// The first button under |root|, or null.
views::Button* FindButton(views::View* root) {
  if (auto* button = views::Button::AsButton(root)) {
    return button;
  }
  for (views::View* child : root->children()) {
    if (views::Button* match = FindButton(child)) {
      return match;
    }
  }
  return nullptr;
}

// A page action bubble, as upstream's derive from LocationBarBubbleDelegateView.
class TestLocationBarBubble : public LocationBarBubbleDelegateView {
 public:
  TestLocationBarBubble(views::View* anchor, content::WebContents* contents)
      : LocationBarBubbleDelegateView(anchor, contents) {}
};

views::View* FindByElementId(views::View* root, ui::ElementIdentifier id) {
  if (root->GetProperty(views::kElementIdentifierKey) == id) {
    return root;
  }
  for (views::View* child : root->children()) {
    if (views::View* match = FindByElementId(child, id)) {
      return match;
    }
  }
  return nullptr;
}

// Like base::test::RunUntil(), but lays the window out first each time:
// layout waits for a frame, and an occluded window (a locked screen, say)
// gets none.
bool RunUntilLaidOut(views::View* view, base::FunctionRef<bool()> condition) {
  return base::test::RunUntil([&] {
    view->GetWidget()->LayoutRootViewIfNecessary();
    return condition();
  });
}

void Click(views::Button* button) {
  views::test::ButtonTestApi(button).NotifyClick(ui::MouseEvent(
      ui::EventType::kMousePressed, gfx::Point(), gfx::Point(),
      ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON, 0));
}

class SidebarHeaderBrowserTest : public InProcessBrowserTest {
 protected:
  // Motions end as they start. With frames (an unlocked screen), Reset()
  // would end a running motion without stopping its animator, whose next
  // frame then fails a CHECK.
  explicit SidebarHeaderBrowserTest(
      gfx::Animation::RichAnimationRenderMode animation_mode =
          gfx::Animation::RichAnimationRenderMode::FORCE_DISABLED)
      : animation_mode_(
            gfx::AnimationTestApi::SetRichAnimationRenderMode(animation_mode)) {}

  gfx::AnimationTestApi::RenderModeResetter animation_mode_;

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  views::View* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }
  SidebarHeader* header() {
    return FindDescendant<SidebarHeader>(browser_view());
  }
  std::u16string pill_text() {
    return std::u16string(header()->pill_for_testing()->GetText());
  }
  BrowserView* browser_view_impl() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }
  // The toolbar controls the header takes.
  std::vector<views::View*> toolbar_controls() {
    return {FindByElementId(browser_view(),
                            kToolbarExtensionsContainerElementId),
            FindByElementId(browser_view(),
                            kPinnedToolbarActionsContainerElementId),
            browser_view()->GetViewByID(VIEW_ID_APP_MENU)};
  }
  // Collapses or expands the sidebar at once: the animation runs on the
  // compositor, and an occluded window (a locked screen, say) gets no frames.
  void Collapse(VerticalTabStripRegionView* region, bool collapse) {
    region->RequestCollapse(collapse);
    BrowserAnimationController::From(browser())->Reset(
        TabStripAnimations::kVerticalTabStrip,
        collapse ? TabStripAnimations::kCollapse : TabStripAnimations::kExpand);
  }
  void SetVerticalTabs(bool enabled) {
    browser()->GetProfile()->GetPrefs()->SetBoolean(prefs::kVerticalTabsEnabled,
                                                 enabled);
  }
};

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, ShowsNavButtonsAndPill) {
  ASSERT_TRUE(header());
  views::View* back = header()->back_button_for_testing();
  views::View* forward = header()->forward_button_for_testing();
  views::View* reload = header()->reload_button_for_testing();
  views::View* pill = header()->pill_for_testing();
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return !pill->bounds().IsEmpty(); }));

  for (views::View* view : {back, forward, reload, pill}) {
    EXPECT_TRUE(view->GetVisible());
    EXPECT_FALSE(view->bounds().IsEmpty());
  }
  // One row, in order, on the collapse button's line, and the pill under it.
  const int row_center =
      header()->GetCollapseButton()->bounds().CenterPoint().y();
  EXPECT_EQ(back->bounds().CenterPoint().y(), row_center);
  EXPECT_LT(back->bounds().right(), forward->bounds().x());
  EXPECT_LT(forward->bounds().right(), reload->bounds().x());
  EXPECT_GT(back->bounds().x(), header()->GetCollapseButton()->bounds().right());
  EXPECT_GE(pill->y(), reload->bounds().bottom());
  // The app menu follows it on its row.
  EXPECT_EQ(pill->x(), 0);
  EXPECT_LT(pill->bounds().right(), header()->width());
  // Upstream's combo button is unpinned by default.
  EXPECT_TRUE(header()->GetComboButton()->bounds().IsEmpty() ||
              !header()->GetComboButton()->GetVisible());
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, BackAndForward) {
  const GURL first = embedded_test_server()->GetURL("/title1.html");
  const GURL second = embedded_test_server()->GetURL("/title2.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), first));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), second));
  views::Button* back = header()->back_button_for_testing();
  views::Button* forward = header()->forward_button_for_testing();
  EXPECT_TRUE(back->GetEnabled());
  EXPECT_FALSE(forward->GetEnabled());

  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  content::TestNavigationObserver back_observer(contents);
  Click(back);
  back_observer.Wait();
  EXPECT_EQ(contents->GetLastCommittedURL(), first);
  EXPECT_TRUE(RunUntilLaidOut(browser_view(), [&] { return forward->GetEnabled(); }));

  content::TestNavigationObserver forward_observer(contents);
  Click(forward);
  forward_observer.Wait();
  EXPECT_EQ(contents->GetLastCommittedURL(), second);
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, PillFollowsTheActiveTab) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("www.a.test", "/title1.html")));
  EXPECT_EQ(pill_text(), u"a.test");

  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  EXPECT_EQ(pill_text(), u"Search or enter URL");

  browser()->tab_strip_model()->ActivateTabAt(0);
  EXPECT_EQ(pill_text(), u"a.test");
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, PillOpensTheCommandBar) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  CommandBar* bar = CommandBar::From(browser());
  ASSERT_TRUE(bar);

  Click(header()->pill_for_testing());

  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return bar->IsShowing(); }));
  EXPECT_EQ(content::EvalJs(bar->web_contents_for_testing(), R"(
      document.querySelector('strata-command-bar-app').shadowRoot
          .querySelector('#input').value)"),
            url.spec());
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, CollapsingHidesTheHeader) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  views::View* pill = header()->pill_for_testing();
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return pill->GetVisible(); }));

  Collapse(region, true);
  EXPECT_TRUE(RunUntilLaidOut(browser_view(), [&] {
    return !pill->GetVisible() &&
           !header()->back_button_for_testing()->GetVisible();
  }));

  Collapse(region, false);
  EXPECT_TRUE(RunUntilLaidOut(browser_view(), [&] { return pill->GetVisible(); }));
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, HoldsTheToolbarControls) {
  ToolbarView* toolbar = browser_view_impl()->toolbar();
  EXPECT_FALSE(browser_view_impl()->IsToolbarVisible());
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return !toolbar->GetVisible(); }));
  EXPECT_EQ(toolbar->height(), 0);

  views::View* app_menu = browser_view()->GetViewByID(VIEW_ID_APP_MENU);
  ASSERT_TRUE(app_menu);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return app_menu->IsDrawn(); }));
  for (views::View* view : toolbar_controls()) {
    ASSERT_TRUE(view);
    EXPECT_TRUE(header()->Contains(view));
    EXPECT_FALSE(toolbar->Contains(view));
  }
  // A container upstream hides leaves no gap.
  for (views::View* view : toolbar_controls()) {
    if (!view->GetVisible()) {
      EXPECT_TRUE(!view->parent()->GetVisible() ||
                  view->parent()->bounds().IsEmpty());
    }
  }
  // On the pill's row, after the pill.
  views::View* pill = header()->pill_for_testing();
  gfx::Rect menu_bounds = app_menu->GetBoundsInScreen();
  EXPECT_GE(menu_bounds.x(), pill->GetBoundsInScreen().right());
  EXPECT_EQ(menu_bounds.CenterPoint().y(),
            pill->GetBoundsInScreen().CenterPoint().y());
  // Bubbles that ask for the app menu anchor in the sidebar.
  EXPECT_EQ(browser_view_impl()
                ->toolbar_button_provider()
                ->GetAppMenuControl()
                ->GetAnchor()
                .GetIfView(),
            app_menu);
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest,
                       HorizontalTabsGiveTheControlsBack) {
  ToolbarView* toolbar = browser_view_impl()->toolbar();
  const std::vector<views::View*> controls = toolbar_controls();

  SetVerticalTabs(false);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return toolbar->IsDrawn(); }));
  EXPECT_TRUE(browser_view_impl()->IsToolbarVisible());
  EXPECT_GT(toolbar->height(), 0);
  for (views::View* view : controls) {
    EXPECT_EQ(view->parent(), toolbar);
  }
  // In upstream's order: the app menu stays at the end.
  EXPECT_GT(toolbar->GetIndexOf(controls[2]).value(),
            toolbar->GetIndexOf(controls[1]).value());
  EXPECT_GT(toolbar->GetIndexOf(controls[1]).value(),
            toolbar->GetIndexOf(controls[0]).value());

  SetVerticalTabs(true);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return !toolbar->GetVisible(); }));
  for (views::View* view : controls) {
    EXPECT_TRUE(header()->Contains(view));
  }
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, SiteInfoOpensPageInfo) {
  views::ImageButton* site_info = header()->site_info_button_for_testing();
  // The new tab page has no page info.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return !site_info->GetVisible(); }));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("a.test", "/title1.html")));
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return site_info->IsDrawn(); }));
  EXPECT_TRUE(header()->pill_for_testing()->GetBoundsInScreen().Contains(
      site_info->GetBoundsInScreen()));

  Click(site_info);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] {
    return PageInfoBubbleViewBase::GetPageInfoBubbleForTesting();
  }));
  EXPECT_EQ(PageInfoBubbleViewBase::GetPageInfoBubbleForTesting()
                ->GetAnchorView(),
            site_info);
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest, CollapsedShowsOnlyTheAppMenu) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  const std::vector<views::View*> controls = toolbar_controls();
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return controls[2]->IsDrawn(); }));

  Collapse(region, true);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), 
      [&] { return !header()->pill_for_testing()->GetVisible(); }));
  EXPECT_TRUE(controls[2]->IsDrawn());
  EXPECT_FALSE(controls[0]->IsDrawn());
  EXPECT_FALSE(controls[1]->IsDrawn());
  EXPECT_FALSE(header()->site_info_button_for_testing()->IsDrawn());
  // Still hidden, and its controls still here.
  EXPECT_FALSE(browser_view_impl()->IsToolbarVisible());
  EXPECT_TRUE(header()->Contains(controls[0]));

  Collapse(region, false);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), 
      [&] { return header()->pill_for_testing()->GetVisible(); }));
  // Drawn again wherever upstream shows them with some width (without
  // extensions or pinned actions, as in a test profile, the containers hide
  // or shrink to nothing).
  for (views::View* view : controls) {
    EXPECT_EQ(view->IsDrawn(),
              view->GetVisible() && !view->GetPreferredSize().IsEmpty())
        << view->GetClassName();
  }
  EXPECT_TRUE(controls[2]->IsDrawn());
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest,
                       AnchoredBubbleExpandsTheCollapsedSidebar) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  // The extensions container holds a button even without extensions.
  views::Button* button = FindButton(toolbar_controls()[0]);
  ASSERT_TRUE(button);
  Collapse(region, true);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] {
    return !header()->pill_for_testing()->GetVisible();
  }));
  ASSERT_FALSE(region->is_expanded_on_hover());

  // As a bubble anchoring to it does (the downloads bubble, an extension's
  // popup).
  std::optional<views::Button::ScopedAnchorHighlight> highlight(
      button->AddAnchorHighlight());
  EXPECT_TRUE(region->is_expanded_on_hover());
  // Laid out as expanded, so the button's slot shows again.
  EXPECT_TRUE(RunUntilLaidOut(browser_view(), [&] {
    return header()->pill_for_testing()->IsDrawn();
  }));

  highlight.reset();
  EXPECT_FALSE(region->is_expanded_on_hover());
  EXPECT_TRUE(RunUntilLaidOut(browser_view(), [&] {
    return !header()->pill_for_testing()->GetVisible();
  }));
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest,
                       PermissionPromptsAnchorToSiteInfo) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  views::ImageButton* site_info = header()->site_info_button_for_testing();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("a.test", "/title1.html")));
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return site_info->IsDrawn(); }));

  bubble_anchor_util::AnchorConfiguration config =
      bubble_anchor_util::GetPageInfoAnchorConfiguration(browser());
  EXPECT_EQ(config.anchor.GetIfView(), site_info);
  EXPECT_EQ(config.highlighted_element,
            SidebarHeader::kSiteInfoButtonElementId);
  EXPECT_EQ(config.bubble_arrow, views::BubbleBorder::TOP_LEFT);

  // Collapsed, upstream's fallback: the app menu, which stays drawn.
  Collapse(region, true);
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return !site_info->IsDrawn(); }));
  config = bubble_anchor_util::GetPageInfoAnchorConfiguration(browser());
  EXPECT_EQ(config.anchor.GetIfView(),
            browser_view()->GetViewByID(VIEW_ID_APP_MENU));
}

IN_PROC_BROWSER_TEST_F(SidebarHeaderBrowserTest,
                       PageActionBubblesAnchorToThePill) {
  views::View* pill = header()->pill_for_testing();
  ToolbarButtonProvider* toolbar = browser_view_impl()->toolbar_button_provider();
  ASSERT_TRUE(RunUntilLaidOut(browser_view(), [&] { return pill->IsDrawn(); }));

  // Upstream's fallback for a location bar that isn't drawn.
  EXPECT_EQ(toolbar->GetBubbleAnchor(std::nullopt).GetIfView(), pill);
  // A location bar icon.
  EXPECT_EQ(toolbar->GetPageActionBubbleAnchor(kActionBookmarkThisTab)
                .GetIfView(),
            pill);
  // A page action bubble anchored in the hidden location bar.
  TestLocationBarBubble bubble(browser_view_impl()->GetLocationBarView(),
                               browser()->tab_strip_model()->GetActiveWebContents());
  EXPECT_EQ(bubble.GetAnchorView(), pill);
  EXPECT_EQ(bubble.arrow(), views::BubbleBorder::TOP_LEFT);

  // Pinned toolbar actions are drawn in the header: theirs stay.
  views::View* app_menu = browser_view()->GetViewByID(VIEW_ID_APP_MENU);
  EXPECT_EQ(GetToolbarBubbleAnchor(views::BubbleAnchor(app_menu)).GetIfView(),
            app_menu);
}

// Animated, as in the app: the extensions container animates its size.
class SidebarHeaderAnimatedBrowserTest : public SidebarHeaderBrowserTest {
 protected:
  SidebarHeaderAnimatedBrowserTest()
      : SidebarHeaderBrowserTest(
            gfx::Animation::RichAnimationRenderMode::FORCE_ENABLED) {}

  // An extension with an action, kept in the profile so it loads again
  // after the restart.
  base::FilePath WriteExtension() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    base::FilePath dir =
        browser()->GetProfile()->GetPath().AppendASCII("pinned_extension");
    EXPECT_TRUE(base::CreateDirectory(dir));
    EXPECT_TRUE(base::WriteFile(dir.AppendASCII("manifest.json"), R"({
      "name": "Pinned", "version": "1", "manifest_version": 3,
      "action": {}
    })"));
    return dir;
  }
};

IN_PROC_BROWSER_TEST_F(SidebarHeaderAnimatedBrowserTest,
                       PRE_PinnedExtensionShowsAtStartup) {
  scoped_refptr<const extensions::Extension> extension =
      extensions::ChromeTestExtensionLoader(browser()->GetProfile())
          .LoadExtension(WriteExtension());
  ASSERT_TRUE(extension);
  // Test builds may pin new extensions already.
  ToolbarActionsModel* model = ToolbarActionsModel::Get(browser()->GetProfile());
  if (!model->IsActionPinned(extension->id())) {
    model->SetActionVisibility(extension->id(), true);
  }
}

// Already pinned when the window opens, the icon starts at zero width. The
// container only grows into the space its parent says is available: its
// slot's own size would keep it there.
IN_PROC_BROWSER_TEST_F(SidebarHeaderAnimatedBrowserTest,
                       PinnedExtensionShowsAtStartup) {
  views::View* extensions = toolbar_controls()[0];
  ToolbarActionView* icon = nullptr;
  EXPECT_TRUE(RunUntilLaidOut(browser_view(), [&] {
    icon = FindDescendant<ToolbarActionView>(extensions);
    return icon && icon->IsDrawn() && icon->width() > 0;
  }));
  ASSERT_TRUE(icon);
  EXPECT_TRUE(
      header()->GetBoundsInScreen().Contains(icon->GetBoundsInScreen()));
}

}  // namespace
}  // namespace strata
