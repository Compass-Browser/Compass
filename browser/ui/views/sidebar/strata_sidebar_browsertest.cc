// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/animation/browser_animation_controller.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/animations/tab_strip_animations.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/fade_label_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_bubble_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_bottom_container.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "strata/browser/ui/views/frame/content_frame.h"
#include "strata/browser/ui/views/frame/window_frame.h"
#include "strata/browser/ui/views/sidebar/sidebar_context_menu.h"
#include "strata/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "strata/browser/ui/views/sidebar/sidebar_tint.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/space_service.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/base_window.h"
#include "ui/base/hit_test.h"
#include "ui/base/models/menu_model.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_test_api.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/non_client_view.h"
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

// Waits until the sidebar page has rendered |count| tab rows.
constexpr char kWaitForTabRows[] = R"(
  (async (count) => {
    const rows = () => document.querySelector('strata-sidebar-app')
        ?.shadowRoot?.querySelectorAll('.tab') ?? [];
    while (rows().length !== count) {
      await new Promise(r => setTimeout(r, 20));
    }
    return [...rows()].map(r => r.title);
  })($1))";

class StrataSidebarBrowserTest : public InProcessBrowserTest {
 protected:
  // Motions end as they start. With frames (an unlocked screen), Reset()
  // would end a running motion without stopping its animator, whose next
  // frame then fails a CHECK.
  gfx::AnimationTestApi::RenderModeResetter animation_mode_ =
      gfx::AnimationTestApi::SetRichAnimationRenderMode(
          gfx::Animation::RichAnimationRenderMode::FORCE_DISABLED);

  views::View* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }

  StrataSidebarView* sidebar() {
    return FindDescendant<StrataSidebarView>(browser_view());
  }

  content::WebContents* sidebar_contents() {
    content::WebContents* contents = sidebar()->GetWebContents();
    EXPECT_TRUE(content::WaitForLoadStop(contents));
    return contents;
  }
};

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ReplacesNativeTabStrip) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  ASSERT_TRUE(sidebar());
  EXPECT_TRUE(region->Contains(sidebar()));
  EXPECT_TRUE(sidebar()->GetVisible());
  EXPECT_EQ(sidebar()->GetWebContents()->GetVisibleURL(),
            GURL("chrome://strata-sidebar/"));
  // The native strip is never built.
  EXPECT_FALSE(FindDescendant<TabStripView>(browser_view()));
  EXPECT_FALSE(region->GetTabStripView());
  EXPECT_GT(sidebar()->height(), 0);
  // Upstream's New Tab button under the strip is hidden: the page has its own.
  auto* bottom = FindDescendant<VerticalTabStripBottomContainer>(region);
  ASSERT_TRUE(bottom);
  EXPECT_FALSE(bottom->GetVisible());
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, TintCoversTheWholeRegion) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  auto* tint = FindDescendant<SidebarTint>(region);
  ASSERT_TRUE(tint);
  // Behind everything else in the region, header included.
  EXPECT_EQ(region->GetIndexOf(tint), 0u);
  views::Widget* widget = browser_view()->GetWidget();
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return !region->bounds().IsEmpty() &&
           tint->bounds() == region->GetLocalBounds();
  }));
  // Down to the region's bottom edge, under the margin below the page.
  EXPECT_GT(tint->bounds().bottom(),
            views::View::ConvertRectToTarget(sidebar(), region,
                                             sidebar()->GetLocalBounds())
                .bottom());

  // Grey spaces aren't tinted; others are, with their color.
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  space.SetColor(SpaceColor::kGrey);
  EXPECT_FALSE(tint->GetTintForTesting());
  space.SetColor(SpaceColor::kBlue);
  ASSERT_TRUE(tint->GetTintForTesting());
  EXPECT_EQ(SkColorSetA(*tint->GetTintForTesting(), SK_AlphaOPAQUE),
            *GetSpaceTint(SpaceColor::kBlue));
  EXPECT_LT(SkColorGetA(*tint->GetTintForTesting()), SK_AlphaOPAQUE);

  // It follows the region when it resizes.
  const int width = region->width();
  region->OnResize(40, /*done_resizing=*/true);
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return region->width() != width &&
           tint->bounds() == region->GetLocalBounds();
  }));
}

// A swipe blends the tint toward the space it pulls in; letting go goes back,
// and switching takes the new space's color. Animations are off here, so
// fades end at once.
IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, TintFollowsASwipe) {
  auto* tint = FindDescendant<SidebarTint>(browser_view());
  ASSERT_TRUE(tint);
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& home = features->space_model();
  home.SetColor(SpaceColor::kBlue);
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  work->SetColor(SpaceColor::kGreen);
  const SkColor blue = *tint->GetTintForTesting();

  features->SetSpacePreview(work, 0.5);
  ASSERT_TRUE(tint->GetTintForTesting());
  const SkColor halfway = *tint->GetTintForTesting();
  EXPECT_NE(halfway, blue);
  EXPECT_GT(SkColorGetG(halfway), SkColorGetG(blue));
  EXPECT_LT(SkColorGetB(halfway), SkColorGetB(blue));

  features->SetSpacePreview(nullptr, 0);
  EXPECT_EQ(*tint->GetTintForTesting(), blue);

  // A grey space has no tint: the blend fades it out.
  work->SetColor(SpaceColor::kGrey);
  features->SetSpacePreview(work, 0.5);
  EXPECT_LT(SkColorGetA(*tint->GetTintForTesting()), SkColorGetA(blue));

  work->SetColor(SpaceColor::kGreen);
  features->SwitchToSpace(*work);
  EXPECT_FALSE(features->preview_space());
  ASSERT_TRUE(tint->GetTintForTesting());
  EXPECT_EQ(SkColorSetA(*tint->GetTintForTesting(), SK_AlphaOPAQUE),
            *GetSpaceTint(SpaceColor::kGreen));
  EXPECT_FALSE(tint->IsAnimatingForTesting());
}

// The space color seeds the window's theme: its frame color follows the
// space, not during a swipe, and a grey space keeps the profile's theme.
IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, WindowThemeFollowsTheSpace) {
  views::Widget* widget = browser_view()->GetWidget();
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& home = features->space_model();
  auto frame_color = [&] {
    return widget->GetColorProvider()->GetColor(ui::kColorFrameActive);
  };
  // The sidebar page's colors, from chrome://theme/colors.css.
  content::WebContents* contents = sidebar_contents();
  constexpr char kActiveRowColor[] = R"(
    getComputedStyle(document.documentElement)
        .getPropertyValue('--color-tab-background-active-frame-active'))";
  ASSERT_EQ(home.color(), SpaceColor::kGrey);
  EXPECT_FALSE(widget->user_color_override());
  const SkColor grey_frame = frame_color();
  const std::string grey_row =
      content::EvalJs(contents, kActiveRowColor).ExtractString();
  ASSERT_FALSE(grey_row.empty());

  home.SetColor(SpaceColor::kBlue);
  EXPECT_EQ(widget->user_color_override(), GetSpaceTint(SpaceColor::kBlue));
  const SkColor blue_frame = frame_color();
  EXPECT_NE(blue_frame, grey_frame);
  // Bluish.
  EXPECT_GT(SkColorGetB(blue_frame), SkColorGetR(blue_frame));
  // The page reloads its colors (ColorChangeUpdater).
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(contents, kActiveRowColor).ExtractString() !=
           grey_row;
  }));

  // A swipe toward another space leaves the theme alone; the switch takes
  // the new space's.
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  work->SetColor(SpaceColor::kGreen);
  features->SetSpacePreview(work, 0.5);
  EXPECT_EQ(widget->user_color_override(), GetSpaceTint(SpaceColor::kBlue));
  features->SwitchToSpace(*work);
  EXPECT_EQ(widget->user_color_override(), GetSpaceTint(SpaceColor::kGreen));
  EXPECT_GT(SkColorGetG(frame_color()), SkColorGetB(frame_color()));

  // Grey: the profile's theme again.
  work->SetColor(SpaceColor::kGrey);
  EXPECT_FALSE(widget->user_color_override());
  EXPECT_EQ(frame_color(), grey_frame);

  // Off the record keeps its own colors.
  BrowserWindowInterface* incognito = CreateIncognitoBrowser();
  WindowFeatures* incognito_features = WindowFeatures::From(incognito);
  ASSERT_TRUE(incognito_features);
  incognito_features->space_model().SetColor(SpaceColor::kBlue);
  EXPECT_FALSE(BrowserView::GetBrowserViewForBrowser(incognito)
                   ->GetWidget()
                   ->user_color_override());
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, CollapsesAndResizes) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  const int expanded_width = region->width();
  ASSERT_GT(expanded_width, 0);

  // Upstream's resize handle and collapse animation used the native strip.
  region->OnResize(40, /*done_resizing=*/false);
  region->OnResize(40, /*done_resizing=*/true);
  // Layout and the collapse animation wait for frames, which an occluded
  // window (a locked screen, say) doesn't get: lay out by hand, and snap the
  // animation to its end.
  views::Widget* widget = browser_view()->GetWidget();
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return region->width() > expanded_width;
  }));

  const int resized_width = region->width();
  region->RequestCollapse(true);
  BrowserAnimationController::From(browser())->Reset(
      TabStripAnimations::kVerticalTabStrip, TabStripAnimations::kCollapse);
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return region->width() < resized_width;
  }));
  // Only the window frame's edge.
  EXPECT_EQ(region->width(), kCollapsedSidebarWidth);
  EXPECT_FALSE(sidebar()->IsDrawn());
  EXPECT_FALSE(
      FindDescendant<VerticalTabStripBottomContainer>(region)->GetVisible());

  region->RequestCollapse(false);
  BrowserAnimationController::From(browser())->Reset(
      TabStripAnimations::kVerticalTabStrip, TabStripAnimations::kExpand);
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return region->width() == resized_width;
  }));
  EXPECT_TRUE(sidebar()->IsDrawn());
}

// The page sits in a round-cornered card inside a frame of the space's color,
// which drags the window. Collapsed, the sidebar is the frame's leading edge
// and the caption buttons hide.
IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, WindowFrame) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  views::Widget* widget = browser_view()->GetWidget();
  MultiContentsView* card =
      BrowserView::GetBrowserViewForBrowser(browser())->multi_contents_view();
  auto layout = [&] {
    widget->LayoutRootViewIfNecessary();
    return card->bounds();
  };

  gfx::Rect bounds = layout();
  EXPECT_EQ(bounds.x(), region->bounds().right());
  EXPECT_GE(bounds.y(), kContentFrameInset);
  EXPECT_EQ(browser_view()->width() - bounds.right(), kContentFrameInset);
  EXPECT_EQ(browser_view()->height() - bounds.bottom(), kContentFrameInset);
  EXPECT_EQ(card->GetBackgroundRadii(),
            gfx::RoundedCornersF(kContentCornerRadius));

  // The frame is tinted as the sidebar is, from under the browser view's
  // children.
  WindowFeatures::From(browser())->space_model().SetColor(SpaceColor::kBlue);
  auto* frame_tint =
      views::AsViewClass<SidebarTint>(browser_view()->children().front());
  ASSERT_TRUE(frame_tint);
  EXPECT_EQ(frame_tint->bounds(), browser_view()->GetLocalBounds());
  EXPECT_TRUE(frame_tint->GetTintForTesting());
  EXPECT_EQ(frame_tint->GetTintForTesting(),
            FindDescendant<SidebarTint>(region)->GetTintForTesting());

  // The frame drags the window; the page doesn't.
  auto hit_test = [&](gfx::Point point) {
    views::View::ConvertPointToWidget(browser_view(), &point);
    return widget->non_client_view()->NonClientHitTest(point);
  };
  EXPECT_EQ(hit_test(gfx::Point(bounds.CenterPoint().x(),
                                browser_view()->height() - 2)),
            HTCAPTION);
  EXPECT_EQ(hit_test(gfx::Point(browser_view()->width() - 2,
                                bounds.CenterPoint().y())),
            HTCAPTION);
  EXPECT_EQ(hit_test(bounds.CenterPoint()), HTCLIENT);

  WindowFrame* frame = WindowFrame::FromRegion(region);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->AreCaptionButtonsHiddenForTesting());
  region->RequestCollapse(true);
  BrowserAnimationController::From(browser())->Reset(
      TabStripAnimations::kVerticalTabStrip, TabStripAnimations::kCollapse);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return layout().x() == kCollapsedSidebarWidth; }));
  EXPECT_EQ(region->bounds(), gfx::Rect(0, 0, kCollapsedSidebarWidth,
                                        browser_view()->height()));
  EXPECT_TRUE(frame->AreCaptionButtonsHiddenForTesting());

  region->RequestCollapse(false);
  BrowserAnimationController::From(browser())->Reset(
      TabStripAnimations::kVerticalTabStrip, TabStripAnimations::kExpand);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return layout().x() > kCollapsedSidebarWidth; }));
  EXPECT_FALSE(frame->AreCaptionButtonsHiddenForTesting());
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ListsTabs) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  EXPECT_EQ(browser()->tab_strip_model()->count(), 2);
  EXPECT_EQ(content::EvalJs(sidebar_contents(),
                            content::JsReplace(kWaitForTabRows, 2))
                .ExtractList()
                .size(),
            2u);
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ClickingATabActivatesIt) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_EQ(model->active_index(), 1);
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kWaitForTabRows, 2)));

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelectorAll('.tab')[0].click();)"));

  EXPECT_TRUE(base::test::RunUntil([&] { return model->active_index() == 0; }));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest,
                       NewTabButtonOpensTheCommandBar) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kWaitForTabRows, 1)));

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#newTab').click();)"));

  CommandBar* bar = CommandBar::From(browser());
  ASSERT_TRUE(bar);
  EXPECT_TRUE(base::test::RunUntil([&] { return bar->IsShowing(); }));
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
}

// Waits until the sidebar's tab rows have the titles |titles|, in order.
constexpr char kWaitForTitles[] = R"(
  (async (titles) => {
    const current = () => [...(document.querySelector('strata-sidebar-app')
        ?.shadowRoot?.querySelectorAll('.tab') ?? [])].map(r => r.title);
    while (JSON.stringify(current()) !== JSON.stringify(titles)) {
      await new Promise(r => setTimeout(r, 20));
    }
    return true;
  })($1))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest,
                       ModelMovesReorderSidebarAndStrip) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Two</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(
                              kWaitForTitles,
                              base::ListValue().Append("One").Append("Two"))));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  model.Move(model.today_root()->children()[1].get(), model.today_root(), 0);

  EXPECT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(
                              kWaitForTitles,
                              base::ListValue().Append("Two").Append("One"))));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return browser()->tab_strip_model()->GetWebContentsAt(0)->GetTitle() ==
           u"Two";
  }));
}

// Drags the entry titled $1 onto $2, a CSS selector in the sidebar's shadow
// root, at fraction $3 of the target's height, using synthetic drag events.
constexpr char kDrag[] = R"(
  (async (title, selector, fraction) => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    const until = async (f) => {
      while (!f()) await new Promise(r => setTimeout(r, 20));
      return f();
    };
    const row = await until(() => [...root.querySelectorAll('.row')]
        .find(r => r.title === title || r.textContent.trim() === title));
    const target = await until(() => root.querySelector(selector));
    const rect = target.getBoundingClientRect();
    const init = {
      bubbles: true, cancelable: true, composed: true,
      dataTransfer: new DataTransfer(),
      clientX: rect.left + rect.width / 2,
      clientY: rect.top + rect.height * fraction,
    };
    row.dispatchEvent(new DragEvent('dragstart', init));
    target.dispatchEvent(new DragEvent('dragover', init));
    target.dispatchEvent(new DragEvent('drop', init));
    row.dispatchEvent(new DragEvent('dragend', init));
    return true;
  })($1, $2, $3))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, DragToPinnedPinsATab) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Two</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(
                              kWaitForTitles,
                              base::ListValue().Append("One").Append("Two"))));

  ASSERT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kDrag, "Two", "#pinned", 0.5)));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return model.pinned_root()->children().size() == 1u; }));
  EXPECT_EQ(model.pinned_root()->children()[0]->title(), u"Two");
  EXPECT_TRUE(base::test::RunUntil([&] {
    return browser()->tab_strip_model()->GetWebContentsAt(0)->GetTitle() ==
           u"Two";
  }));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest,
                       FavoritesStayAcrossSpacesAndReopen) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Two</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(
                              kWaitForTitles,
                              base::ListValue().Append("One").Append("Two"))));

  ASSERT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kDrag, "Two", "#favorites",
                                                0.5)));

  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& favorites = features->service().space_list().favorites();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return favorites.pinned_root()->children().size() == 1u; }));
  SidebarNode* two = favorites.pinned_root()->children()[0].get();
  EXPECT_EQ(two->title(), u"Two");
  TabStripModel* tabs = browser()->tab_strip_model();
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return tabs->GetWebContentsAt(0)->GetTitle() == u"Two"; }));
  EXPECT_EQ(content::EvalJs(contents, R"(
      document.querySelector('strata-sidebar-app').shadowRoot
          .querySelector('.tile').title)"),
            "Two");

  // Another space: One leaves the window, the favorite stays.
  SpaceList& list = features->service().space_list();
  features->SwitchToSpace(*list.AddSpace(1, u"Work"));
  EXPECT_TRUE(features->tab_strip_bridge().GetTab(two));

  // Closed, it stays unloaded; clicking its tile opens it again.
  tabs->CloseWebContentsAt(
      tabs->GetIndexOfTab(features->tab_strip_bridge().GetTab(two)),
      CLOSE_NONE);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !features->tab_strip_bridge().GetTab(two) &&
           content::EvalJs(contents, R"(
               document.querySelector('strata-sidebar-app').shadowRoot
                   .querySelector('.tile').hasAttribute('unloaded'))")
                   .ExtractBool();
  }));
  ASSERT_TRUE(content::ExecJs(contents, R"(
      document.querySelector('strata-sidebar-app').shadowRoot
          .querySelector('.tile').click())"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return features->tab_strip_bridge().GetTab(two); }));
  EXPECT_EQ(tabs->GetActiveTab(), features->tab_strip_bridge().GetTab(two));
  EXPECT_EQ(tabs->GetIndexOfTab(tabs->GetActiveTab()), 0);
}

// Drops a link on |selector| in the sidebar, the way dragging one in from a
// page or another app does.
constexpr char kDropLink[] = R"(
  (async (url, selector) => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    let target;
    while (!(target = root.querySelector(selector))) {
      await new Promise(r => setTimeout(r, 20));
    }
    const rect = target.getBoundingClientRect();
    const dataTransfer = new DataTransfer();
    dataTransfer.setData('text/uri-list', url);
    const init = {
      bubbles: true, cancelable: true, composed: true, dataTransfer,
      clientX: rect.left + rect.width / 2,
      clientY: rect.top + rect.height / 2,
    };
    const accepted = !target.dispatchEvent(new DragEvent('dragover', init));
    target.dispatchEvent(new DragEvent('drop', init));
    return accepted;
  })($1, $2))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, DroppingALinkOpensIt) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  content::WebContents* contents = sidebar_contents();

  ASSERT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kDropLink, url, "#pinned")));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return model.pinned_root()->children().size() == 1u; }));
  EXPECT_EQ(model.pinned_root()->children()[0]->url(), url);
  TabStripModel* tabs = browser()->tab_strip_model();
  EXPECT_EQ(tabs->count(), 2);
  EXPECT_EQ(tabs->GetActiveWebContents()->GetVisibleURL(), url);
  // The sidebar itself didn't navigate.
  EXPECT_EQ(contents->GetLastCommittedURL(), GURL("chrome://strata-sidebar/"));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, SwitchesSpaces) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* one = browser()->tab_strip_model()->GetWebContentsAt(0);
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  content::WebContents* contents = sidebar_contents();

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#addSpace').click();
  )"));

  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  EXPECT_EQ(features->space_model().name(), u"Space 2");
  EXPECT_NE(browser()->tab_strip_model()->GetActiveWebContents(), one);
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      const name = () => root.querySelector('#spaceName')?.textContent.trim();
      while (name() !== 'Space 2' ||
             root.querySelectorAll('.space').length !== 2) {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));
  // Something worth keeping in the new space, so it's parked when we leave.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>Two</title>")));

  // Back to the first space: the same tab, kept alive while hidden. That
  // empties the parking window, and Two goes to a new one.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('.space').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() == home; }));
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), one);

  // And again.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelectorAll('.space')[1].click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('.space').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() == home; }));
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), one);
}

// Switches |browser|'s window to its space at |index| (0 to 8) with the
// keyboard shortcut. Returns whether the shortcut was taken.
bool PressSpaceShortcut(views::View* browser_view, int index) {
#if BUILDFLAG(IS_MAC)
  constexpr int kModifiers = ui::EF_CONTROL_DOWN;
#else
  constexpr int kModifiers = ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN;
#endif
  return browser_view->GetFocusManager()->ProcessAccelerator(ui::Accelerator(
      static_cast<ui::KeyboardCode>(ui::VKEY_1 + index), kModifiers));
}

// Slice 3 of docs/design/spaces.md: a space of another profile swaps the
// window, and the tabs left behind wait in their profile's parking window.
IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest,
                       AnotherProfilesSpaceSwapsTheWindow) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* one = browser()->tab_strip_model()->GetWebContentsAt(0);
  Profile* profile = browser()->GetProfile();
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  // Listed as if it had a window, with its Home.
  SpaceDirectory::Get()->List(&other);
  SpaceModel& other_home = SpaceService::Get(&other)->space_list().active_space();
  const gfx::Rect bounds = browser()->GetWindow()->GetRestoredBounds();
  content::WebContents* contents = sidebar_contents();

  // The switcher lists both profiles' spaces; click the other's. Its ID
  // changes once the profile's file has loaded, so look it up each time.
  constexpr char kSpaceButton[] = R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('.space[data-space-id="' + $1 + '"]'))";
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(
               contents,
               "!!" + content::JsReplace(kSpaceButton,
                                         other_home.id().AsLowercaseString()))
        .ExtractBool();
  }));
  ASSERT_TRUE(content::ExecJs(
      contents, content::JsReplace(kSpaceButton,
                                   other_home.id().AsLowercaseString()) +
                    ".click()"));

  BrowserWindowInterface* swapped = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          if (window->GetProfile() == &other &&
              window->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
            swapped = window;
          }
          return !swapped;
        });
    return swapped != nullptr;
  }));
  SetBrowser(swapped);
  EXPECT_EQ(&WindowFeatures::From(swapped)->space_model(), &other_home);
  EXPECT_EQ(swapped->GetWindow()->GetRestoredBounds(), bounds);
  // Once it has painted, the old window empties and closes; its tab waits,
  // alive, in its profile.
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return SpaceService::Get(profile)->parking_window(); }));
  BrowserWindowInterface* parking =
      SpaceService::Get(profile)->parking_window();
  ASSERT_TRUE(base::test::RunUntil([&] {
    int normal = 0;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          normal += window->GetType() == BrowserWindowInterface::TYPE_NORMAL;
          return true;
        });
    return normal == 1;
  }));
  EXPECT_EQ(parking->GetTabStripModel()->GetWebContentsAt(0), one);

  // Ctrl+1 counts across profiles; the first profile's Home is first or
  // after the other's, as the profile menu orders them.
  const std::vector<SpaceDirectory::Entry> spaces =
      GetSwitchableSpaces(swapped);
  ASSERT_EQ(spaces.size(), 2u);
  const int index = spaces[0].profile == profile ? 0 : 1;
  views::View* view = BrowserView::GetBrowserViewForBrowser(swapped);
  ASSERT_TRUE(PressSpaceShortcut(view, index));
  BrowserWindowInterface* back = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          if (window->GetProfile() == profile &&
              window->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
            back = window;
          }
          return !back;
        });
    return back != nullptr;
  }));
  SetBrowser(back);
  EXPECT_EQ(back->GetTabStripModel()->GetActiveWebContents(), one);
}

// Assigning the window's space to another profile from the space menu, as in
// Arc: the window swaps to that profile and reloads the page it showed.
IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, SpaceMenuAssignsAProfile) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  content::WebContents* contents = sidebar_contents();

  ASSERT_TRUE(content::ExecJs(
      contents,
      content::JsReplace(R"(
        (async (id) => {
          const root = document.querySelector('strata-sidebar-app').shadowRoot;
          const until = async (f) => {
            while (!f()) await new Promise(r => setTimeout(r, 20));
            return f();
          };
          root.querySelector('.space').dispatchEvent(new MouseEvent(
              'contextmenu', {bubbles: true, cancelable: true, composed: true}));
          const select = await until(() =>
              [...root.querySelectorAll('#spaceProfile option')]
                  .find(option => option.value === id)?.parentElement);
          select.value = id;
          select.dispatchEvent(new Event('change'));
        })($1))",
                         other.GetBaseName().AsUTF8Unsafe())));

  BrowserWindowInterface* swapped = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          if (window->GetProfile() == &other &&
              window->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
            swapped = window;
          }
          return !swapped;
        });
    return swapped != nullptr;
  }));
  SetBrowser(swapped);
  // The same space, now the other profile's, alone in the switcher: the
  // first profile had no other space, and the new one's own Home stays out.
  SpaceDirectory* directory = SpaceDirectory::Get();
  ASSERT_EQ(directory->GetSpaces().size(), 1u);
  EXPECT_EQ(directory->GetSpaces()[0].profile, &other);
  EXPECT_EQ(&WindowFeatures::From(swapped)->space_model(),
            directory->GetSpaces()[0].space);
  EXPECT_EQ(swapped->GetTabStripModel()->GetActiveWebContents()->GetVisibleURL(),
            GURL("data:text/html,<title>One</title>"));
}


IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest,
                       DragOntoASpaceMovesTheEntryAndShortcutsSwitch) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Two</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* two = browser()->tab_strip_model()->GetActiveWebContents();
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");

  ASSERT_EQ(true, content::EvalJs(sidebar_contents(),
                                  content::JsReplace(kDrag, "Two",
                                                     ".space[title='Work']",
                                                     0.5)));

  // Two moved, with its tab; the window shows One.
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return work->today_root()->children().size() == 1u; }));
  EXPECT_EQ(work->today_root()->children()[0]->title(), u"Two");
  EXPECT_EQ(home->today_root()->children().size(), 1u);
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents()->GetTitle(),
            u"One");

  // Ctrl+2 goes to Work, where Two is still the same page.
  EXPECT_TRUE(PressSpaceShortcut(browser_view(), 1));
  EXPECT_EQ(&features->space_model(), work);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), two);
  EXPECT_TRUE(PressSpaceShortcut(browser_view(), 0));
  EXPECT_EQ(&features->space_model(), home);
  // There's no third space.
  EXPECT_FALSE(PressSpaceShortcut(browser_view(), 2));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, SpaceMenuRenamesColorsDeletes) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#addSpace').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  SpaceModel* added = &features->space_model();

  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      const until = async (f) => {
        while (!f()) await new Promise(r => setTimeout(r, 20));
        return f();
      };
      const button = await until(() => root.querySelectorAll('.space')[1]);
      button.dispatchEvent(new MouseEvent(
          'contextmenu', {bubbles: true, cancelable: true, composed: true}));
      const input = await until(() => root.querySelector('#spaceNameInput'));
      input.value = 'Work';
      input.dispatchEvent(new Event('change'));
      root.querySelector('.swatch[title="Blue"]').click();
      return true;
    })())"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return added->name() == u"Work" && added->color() == SpaceColor::kBlue;
  }));

  // Delete wants a second click. The window goes back to Home.
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      const until = async (f) => {
        while (!f()) await new Promise(r => setTimeout(r, 20));
        return f();
      };
      // Reopened from the name, which also starts over.
      root.querySelector('#spaceName').dispatchEvent(new MouseEvent(
          'dblclick', {bubbles: true, composed: true}));
      const remove = await until(() => root.querySelector('#deleteSpace'));
      remove.click();
      await until(() => remove.hasAttribute('confirming'));
      remove.click();
      return true;
    })())"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return features->service().space_list().spaces().size() == 1u;
  }));
  EXPECT_EQ(&features->space_model(), home);
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, DragIntoAFolder) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(kWaitForTitles,
                                             base::ListValue().Append("One"))));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#newFolder').click();)"));

  ASSERT_EQ(true,
            content::EvalJs(contents, content::JsReplace(kDrag, "One",
                                                         ".folder-row", 0.5)));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return model.pinned_root()->children().size() == 1u &&
           model.pinned_root()->children()[0]->children().size() == 1u;
  }));
  const SidebarNode* folder = model.pinned_root()->children()[0].get();
  EXPECT_EQ(folder->type(), SidebarNode::Type::kFolder);
  EXPECT_EQ(folder->children()[0]->title(), u"One");
}

// Waits until the sidebar page's first folder row says it's collapsed ($1)
// or expanded, with its children hidden or shown to match.
constexpr char kWaitForFolder[] = R"(
  (async () => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    const matches = () => {
      const row = root.querySelector('.folder-row');
      return row &&
          row.getAttribute('aria-expanded') === String(!$1) &&
          !root.querySelector('.folder-children') === $1;
    };
    while (!matches()) {
      await new Promise(r => setTimeout(r, 20));
    }
    return true;
  })())";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, FolderCollapseIsShared) {
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  SidebarNode* folder = model.AddFolder(model.pinned_root(), 0, u"Work");
  model.AddTab(folder, 0, GURL("https://a.test/"), u"Inside");
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true, content::EvalJs(
                      contents, content::JsReplace(kWaitForFolder, false)));

  // Clicking the folder collapses it in the model, which saves it.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('.folder-row').click();)"));
  ASSERT_TRUE(base::test::RunUntil([&] { return folder->collapsed(); }));
  EXPECT_EQ(true, content::EvalJs(contents,
                                  content::JsReplace(kWaitForFolder, true)));

  // Another window shows it collapsed too.
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  content::WebContents* second_contents =
      FindDescendant<StrataSidebarView>(
          BrowserView::GetBrowserViewForBrowser(second))
          ->GetWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(second_contents));
  EXPECT_EQ(true, content::EvalJs(second_contents,
                                  content::JsReplace(kWaitForFolder, true)));

  // Expanding it anywhere expands it everywhere.
  model.SetCollapsed(folder, false);
  EXPECT_EQ(true, content::EvalJs(contents,
                                  content::JsReplace(kWaitForFolder, false)));
  EXPECT_EQ(true, content::EvalJs(second_contents,
                                  content::JsReplace(kWaitForFolder, false)));
}

// Right-clicks the first element matching selector $1 in the sidebar page,
// in its middle. Returns whether the page took the event (its own menu
// doesn't show).
constexpr char kRightClick[] = R"(
  (() => {
    const target = document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector($1);
    const rect = target.getBoundingClientRect();
    return !target.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, composed: true, cancelable: true, button: 2,
      clientX: rect.left + rect.width / 2, clientY: rect.top + rect.height / 2,
    }));
  })())";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ContextMenus) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(kWaitForTitles,
                                             base::ListValue().Append("One"))));
  ui::MenuModel* menu = nullptr;
  gfx::Point point;
  SidebarContextMenu::SetShowCallbackForTesting(base::BindLambdaForTesting(
      [&](ui::MenuModel* model, const gfx::Point& at) {
        menu = model;
        point = at;
      }));
  auto label_index = [&](std::u16string_view label) -> std::optional<size_t> {
    for (size_t i = 0; i < menu->GetItemCount(); ++i) {
      if (menu->GetLabelAt(i) == label) {
        return i;
      }
    }
    return std::nullopt;
  };

  // A tab's menu opens where it was clicked, over its row.
  EXPECT_EQ(true, content::EvalJs(contents,
                                  content::JsReplace(kRightClick, ".tab")));
  ASSERT_TRUE(base::test::RunUntil([&] { return menu != nullptr; }));
  const int row_top = content::EvalJs(contents, R"(
    Math.round(document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('.tab').getBoundingClientRect().top))")
                          .ExtractInt();
  EXPECT_GT(point.y(), row_top);
  EXPECT_TRUE(sidebar()->GetLocalBounds().Contains(point));
  std::optional<size_t> pin = label_index(u"Pin");
  ASSERT_TRUE(pin);
  menu->ActivatedAt(*pin);
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return model.pinned_root()->children().size() == 1u; }));

  // Rename in a folder's menu edits its name in place.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#newFolder').click();)"));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      while (!root.querySelector('.folder-row')) {
        await new Promise(r => setTimeout(r, 20));
      }
    })())"));
  menu = nullptr;
  EXPECT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kRightClick, ".folder-row")));
  ASSERT_TRUE(base::test::RunUntil([&] { return menu != nullptr; }));
  std::optional<size_t> rename = label_index(u"Rename");
  ASSERT_TRUE(rename);
  menu->ActivatedAt(*rename);
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      while (root.activeElement?.className !== 'folder-title-input') {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));

  // Elsewhere, there's no menu, not even the page's own.
  menu = nullptr;
  EXPECT_EQ(true, content::EvalJs(contents,
                                  content::JsReplace(kRightClick, "#newTab")));
  // The next menu is the pinned tab's.
  EXPECT_EQ(true, content::EvalJs(contents,
                                  content::JsReplace(kRightClick, ".tab")));
  ASSERT_TRUE(base::test::RunUntil([&] { return menu != nullptr; }));
  EXPECT_TRUE(label_index(u"Unpin"));

  SidebarContextMenu::SetShowCallbackForTesting({});
}

// Opens three titled tabs, One to Three, and waits for the sidebar to list
// them. Three is active.
void OpenThreeTabs(BrowserWindowInterface* browser, content::WebContents* sidebar) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser, GURL("data:text/html,<title>One</title>")));
  for (const char* title : {"Two", "Three"}) {
    ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser,
        GURL(std::string("data:text/html,<title>") + title + "</title>"),
        WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  }
  ASSERT_EQ(true,
            content::EvalJs(sidebar, content::JsReplace(
                                         kWaitForTitles, base::ListValue()
                                                             .Append("One")
                                                             .Append("Two")
                                                             .Append("Three"))));
}

// Presses |key| on the focused row, and returns the title of the row focused
// after it.
constexpr char kPressKey[] = R"(
  (async (key) => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    root.activeElement.dispatchEvent(new KeyboardEvent('keydown', {
      key, bubbles: true, cancelable: true, composed: true,
    }));
    await new Promise(r => setTimeout(r, 0));
    return root.activeElement?.title ?? '';
  })($1))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, KeyboardNavigation) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_NO_FATAL_FAILURE(OpenThreeTabs(browser(), contents));
  TabStripModel* tabs = browser()->tab_strip_model();
  // Focus events need a focused page.
  contents->Focus();

  // Focusing the list lands on the active entry.
  EXPECT_EQ("Three", content::EvalJs(contents, R"(
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    root.querySelector('#sections').focus();
    root.activeElement.title;)"));
  EXPECT_EQ("Two", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "ArrowUp")));
  EXPECT_EQ("One", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "Home")));
  EXPECT_EQ("Two", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "ArrowDown")));

  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kPressKey, "Enter")));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return tabs->GetActiveWebContents()->GetTitle() == u"Two";
  }));

  // Closing moves the focus to the next row.
  EXPECT_EQ("Three", content::EvalJs(contents,
                                     content::JsReplace(kPressKey, "Delete")));
  EXPECT_TRUE(base::test::RunUntil([&] { return tabs->count() == 2; }));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, FavoritesKeyboardNavigation) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_NO_FATAL_FAILURE(OpenThreeTabs(browser(), contents));
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& space = features->space_model();
  SpaceModel& favorites = features->service().space_list().favorites();
  for (const char16_t* title : {u"One", u"Two"}) {
    SidebarNode* node = nullptr;
    for (const auto& child : space.today_root()->children()) {
      if (child->title() == title) {
        node = child.get();
      }
    }
    ASSERT_TRUE(node);
    MoveEntry(browser()->GetProfile(), node, favorites, favorites.pinned_root(),
              favorites.pinned_root()->children().size());
  }
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(contents, R"(
        document.querySelector('strata-sidebar-app').shadowRoot
            .querySelectorAll('.tile').length)")
               .ExtractInt() == 2;
  }));
  contents->Focus();

  // The list lands on the active entry; Up goes into the grid.
  EXPECT_EQ("Three", content::EvalJs(contents, R"(
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    root.querySelector('#sections').focus();
    root.activeElement.title;)"));
  EXPECT_EQ("Two", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "ArrowUp")));
  EXPECT_EQ("One", content::EvalJs(
                       contents, content::JsReplace(kPressKey, "ArrowLeft")));
  EXPECT_EQ("Two", content::EvalJs(
                       contents, content::JsReplace(kPressKey, "ArrowRight")));
  // Two tiles fit in one row: Down leaves the grid.
  EXPECT_EQ("Three", content::EvalJs(
                         contents, content::JsReplace(kPressKey, "ArrowDown")));
  EXPECT_EQ("One", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "Home")));

  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kPressKey, "Enter")));
  TabStripModel* tabs = browser()->tab_strip_model();
  EXPECT_TRUE(base::test::RunUntil([&] {
    return tabs->GetActiveWebContents()->GetTitle() == u"One";
  }));
}

// Rests the pointer on the |index|th tab row, or on nothing (-1: it leaves
// the list).
constexpr char kHover[] = R"(
  ((index) => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    const sections = root.querySelector('#sections');
    if (index < 0) {
      sections.dispatchEvent(new PointerEvent('pointerleave', {
        pointerType: 'mouse',
      }));
      return;
    }
    root.querySelectorAll('.tab')[index].dispatchEvent(
        new PointerEvent('pointerover', {
          pointerType: 'mouse', bubbles: true, composed: true,
        }));
  })($1))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, HoverCards) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_NO_FATAL_FAILURE(OpenThreeTabs(browser(), contents));
  SidebarHoverCard* card = SidebarHoverCard::FromWebContents(contents);
  ASSERT_TRUE(card);

  ASSERT_TRUE(content::ExecJs(contents, content::JsReplace(kHover, 0)));
  ASSERT_TRUE(base::test::RunUntil([&] { return card->IsShowing(); }));
  views::Widget* widget = card->bubble_for_testing()->GetWidget();
  // A new card shows its tab, not an empty frame.
  EXPECT_EQ(card->bubble_for_testing()->GetTitleViewForTesting()->GetText(),
            u"One");
  // One isn't the active tab: the card has room for its preview.
  EXPECT_TRUE(card->bubble_for_testing()->GetThumbnailViewForTesting());
  EXPECT_TRUE(base::test::RunUntil([&] { return widget->IsVisible(); }));
  // To the sidebar's right; its shadow may overlap the edge.
  EXPECT_GT(widget->GetWindowBoundsInScreen().CenterPoint().x(),
            sidebar()->GetBoundsInScreen().right());
  const int first_y = widget->GetWindowBoundsInScreen().y();

  // Moving down the list moves the card.
  ASSERT_TRUE(content::ExecJs(contents, content::JsReplace(kHover, 1)));
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return widget->GetWindowBoundsInScreen().y() > first_y; }));
  EXPECT_EQ(card->bubble_for_testing()->GetWidget(), widget);
  EXPECT_EQ(card->bubble_for_testing()->GetTitleViewForTesting()->GetText(),
            u"Two");

  ASSERT_TRUE(content::ExecJs(contents, content::JsReplace(kHover, -1)));
  EXPECT_TRUE(base::test::RunUntil([&] { return !card->IsShowing(); }));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ArchivePanel) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_NO_FATAL_FAILURE(OpenThreeTabs(browser(), contents));
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& model = features->space_model();
  // One was last seen two days ago; Three is on screen.
  model.SetLastActive(model.today_root()->children()[0].get(),
                      base::Time::Now() - base::Days(2));

  features->service().ArchiveIdleEntries();

  TabStripModel* tabs = browser()->tab_strip_model();
  ASSERT_TRUE(base::test::RunUntil([&] { return tabs->count() == 2; }));
  ASSERT_EQ(model.archive().size(), 1u);
  EXPECT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kWaitForTitles,
                                                base::ListValue()
                                                    .Append("Two")
                                                    .Append("Three"))));

  // The panel lists it; clicking it brings it back.
  EXPECT_EQ("One", content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      root.querySelector('#archiveButton').click();
      let entry;
      while (!(entry = root.querySelector('.archived'))) {
        await new Promise(r => setTimeout(r, 20));
      }
      const title = entry.title;
      entry.click();
      return title;
    })())"));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return tabs->count() == 3 &&
           tabs->GetActiveWebContents()->GetTitle() == u"One";
  }));
  EXPECT_TRUE(model.archive().empty());
}

// Window sync across real windows (docs/design/sidebar.md, "Window sync").
using StrataWindowSyncBrowserTest = StrataSidebarBrowserTest;

IN_PROC_BROWSER_TEST_F(StrataWindowSyncBrowserTest,
                       ClosingAWindowKeepsItsTabs) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      second, GURL("data:text/html,<title>Two</title>")));
  content::WebContents* two =
      second->GetTabStripModel()->GetActiveWebContents();

  CloseBrowserSynchronously(second);

  // The same page, moved, not reloaded.
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_EQ(model->count(), 2);
  EXPECT_NE(model->GetIndexOfWebContents(two), TabStripModel::kNoTab);
  EXPECT_EQ(model->GetActiveWebContents()->GetTitle(), u"One");
}

IN_PROC_BROWSER_TEST_F(StrataWindowSyncBrowserTest,
                       PlaceholderBringsTheTabBack) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  SidebarNode* one = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    for (const auto& node : model.today_root()->children()) {
      if (node->title() == u"One") {
        one = node.get();
      }
    }
    return one != nullptr;
  }));

  // Open One from the second window: it moves there, and the first window,
  // which has no other tab, shows a placeholder instead of closing.
  WindowFeatures::From(second)->tab_strip_bridge().OpenNode(one);
  ASSERT_EQ(browser()->tab_strip_model()->count(), 1);
  content::WebContents* placeholder =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(placeholder));
  EXPECT_EQ(true, content::EvalJs(placeholder, R"(
    (async () => {
      while (document.querySelector('#title').textContent !== 'One') {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));

  // It shows the tab as it was when it left.
  EXPECT_EQ(true, content::EvalJs(placeholder, R"(
    (async () => {
      const img = document.querySelector('#snapshot');
      while (!img.src) {
        await new Promise(r => setTimeout(r, 20));
      }
      await img.decode();
      return img.src.startsWith('data:image/jpeg') && img.naturalWidth > 0;
    })())"));

  ASSERT_TRUE(content::ExecJs(placeholder,
                              "document.querySelector('#bringHere').click()"));

  EXPECT_TRUE(base::test::RunUntil([&] {
    TabStripModel* first = browser()->tab_strip_model();
    return first->count() == 1 &&
           first->GetActiveWebContents()->GetTitle() == u"One";
  }));
  // Now the second window keeps showing One, as a placeholder.
  EXPECT_EQ(second->GetTabStripModel()->count(), 2);
}

}  // namespace
}  // namespace strata
