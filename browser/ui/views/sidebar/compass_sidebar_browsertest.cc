// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/ui/zoom/chrome_zoom_level_prefs.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/web_contents_delegate.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/threading/thread_restrictions.h"
#include "base/numerics/safe_conversions.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/devtools/devtools_window_testing.h"
#include "chrome/browser/infobars/simple_alert_infobar_creator.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/browser/ui/animation/browser_animation_controller.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state.h"
#include "chrome/browser/ui/views/animations/tab_strip_animations.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/fade_label_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_bubble_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_bottom_container.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "ui/views/test/views_test_utils.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "components/infobars/core/infobar_delegate.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/controllable_http_response.h"
#include "ui/native_theme/mock_os_settings_provider.h"
#include "ui/native_theme/native_theme.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "compass/browser/ui/views/frame/load_bar.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/command_bar/command_bar.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "compass/browser/ui/views/frame/window_frame.h"
#include "compass/browser/ui/views/sidebar/sidebar_context_menu.h"
#include "compass/browser/ui/views/sidebar/sidebar_hover_card.h"
#include "compass/browser/ui/views/sidebar/sidebar_swap.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
#include "compass/browser/ui/webui/sidebar/compass_sidebar_ui.h"
#include "compass/browser/sidebar/sidebar_store.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/base_window.h"
#include "ui/base/hit_test.h"
#include "ui/base/models/menu_model.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_utils.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_container.h"
#include "ui/gfx/animation/animation_test_api.h"
#include "ui/events/types/scroll_types.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/controls/resize_area.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/non_client_view.h"
#include "url/gurl.h"

namespace compass {
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
    const rows = () => document.querySelector('compass-sidebar-app')
        ?.shadowRoot?.querySelectorAll('.tab') ?? [];
    while (rows().length !== count) {
      await new Promise(r => setTimeout(r, 20));
    }
    return [...rows()].map(r => r.title);
  })($1))";

class CompassSidebarBrowserTest : public InProcessBrowserTest {
 protected:
  // Motions end as they start. With frames (an unlocked screen), Reset()
  // would end a running motion without stopping its animator, whose next
  // frame then fails a CHECK.
  CompassSidebarBrowserTest()
      : CompassSidebarBrowserTest(
            gfx::Animation::RichAnimationRenderMode::FORCE_DISABLED) {}
  explicit CompassSidebarBrowserTest(
      gfx::Animation::RichAnimationRenderMode mode)
      : animation_mode_(
            gfx::AnimationTestApi::SetRichAnimationRenderMode(mode)) {}

  gfx::AnimationTestApi::RenderModeResetter animation_mode_;

  views::View* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }

  CompassSidebarView* sidebar() {
    return FindDescendant<CompassSidebarView>(browser_view());
  }

  content::WebContents* sidebar_contents() {
    content::WebContents* contents = sidebar()->GetWebContents();
    EXPECT_TRUE(content::WaitForLoadStop(contents));
    return contents;
  }
};

// A long Pinned and Today scroll together, one under the other: shrunk to
// fit, Pinned's entries spilled over Today's header, and the space's name
// lost height (the gap above Pinned changed between spaces). Scrollbars are
// thin on a transparent track.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, LongPinnedScrollsWithToday) {
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  for (int i = 0; i < 60; ++i) {
    model.AddTab(model.pinned_root(), model.pinned_root()->children().size(),
                 GURL("https://pinned" + base::NumberToString(i) + ".test/"),
                 u"Pinned " + base::NumberToString16(i));
  }
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(content::ExecJs(contents, content::JsReplace(R"(
    (async () => {
      const r = () => document.querySelector('compass-sidebar-app').shadowRoot;
      while (r().querySelectorAll('#pinned .tab').length < $1) {
        await new Promise(resolve => setTimeout(resolve, 20));
      }
    })())", 60)));
  constexpr char kLayout[] = R"(
    (() => {
      const r = document.querySelector('compass-sidebar-app').shadowRoot;
      const sections = r.querySelector('#sections');
      // The list's own Pinned, then Today's header, where layout puts them
      // (entries glide there: their boxes may still be on their way).
      const pinned = sections.querySelector(':scope > #pinned');
      const header = sections.querySelectorAll(':scope > .section-header')[1];
      const style = getComputedStyle(sections);
      // All of Pinned's entries, not just its box (they spilled out of it).
      // The space's name keeps its height beside a long list.
      const name = r.querySelector('#spacePane > #spaceName');
      return [header.offsetTop >= pinned.offsetTop + pinned.scrollHeight,
              sections.scrollHeight > sections.clientHeight,
              style.scrollbarColor.endsWith('rgba(0, 0, 0, 0)'),
              name.offsetHeight >= name.scrollHeight];
    })())";
  EXPECT_EQ(content::EvalJs(contents, kLayout),
            base::ListValue().Append(true).Append(true).Append(true).Append(
                true));
}

// Each space's entries keep their scroll position: the new space's showed
// the old one's, and jumped to it as its preview made way.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, SpacesKeepTheirScroll) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  for (SpaceModel* space : {home, work}) {
    for (int i = 0; i < 60; ++i) {
      space->AddTab(space->pinned_root(),
                    space->pinned_root()->children().size(),
                    GURL("https://pinned" + base::NumberToString(i) + ".test/"),
                    u"Pinned " + base::NumberToString16(i));
    }
  }
  content::WebContents* contents = sidebar_contents();
  // The window's space once its switch has ended and its 60 rows are there,
  // how far its entries are scrolled, and how far the preview of the space
  // on |side| shows its own.
  constexpr char kShown[] = R"(
    (async (side) => {
      const app = document.querySelector('compass-sidebar-app');
      const r = app.shadowRoot;
      const preview = () => r.querySelector(
          'compass-sidebar-preview[side=' + side + ']')?.shadowRoot;
      while (app.slide_ || r.querySelectorAll('#pinned .tab').length !== 60 ||
             preview()?.querySelectorAll('.tab').length !== 60) {
        await new Promise(resolve => setTimeout(resolve, 20));
      }
      await new Promise(resolve => requestAnimationFrame(resolve));
      await new Promise(resolve => requestAnimationFrame(resolve));
      return [app.spaces_.find(s => s.id === app.activeSpaceId_).name,
              r.querySelector('#sections').scrollTop,
              preview().querySelector('#sections').scrollTop];
    })($1))";
  ASSERT_EQ(content::EvalJs(contents, content::JsReplace(kShown, "next")),
            base::ListValue().Append("Home").Append(0).Append(0));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#sections').scrollTop = 300;)"));

  // Work's entries start at the top; Home's preview shows Home's where they
  // were left.
  features->SwitchToSpace(*work);
  EXPECT_EQ(content::EvalJs(contents, content::JsReplace(kShown, "previous")),
            base::ListValue().Append("Work").Append(0).Append(300));
  // And Home's come back there.
  features->SwitchToSpace(*home);
  EXPECT_EQ(content::EvalJs(contents, content::JsReplace(kShown, "next")),
            base::ListValue().Append("Home").Append(300).Append(0));
}

// An empty Today takes no room: New Tab sits right under its header. It left
// a blank gap there, a drop zone that only matters while something is
// dragged, when it comes back.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, AnEmptyTodayTakesNoRoom) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  features->SwitchToSpace(*work);
  ASSERT_TRUE(work->today_root()->children().empty());
  content::WebContents* contents = sidebar_contents();
  // The room between Today's header and New Tab, and Today's own height.
  constexpr char kRoom[] = R"(
    (async () => {
      const app = document.querySelector('compass-sidebar-app');
      const r = app.shadowRoot;
      while (app.spaces_?.find(s => s.id === app.activeSpaceId_)?.name !==
                 'Work' ||
             !r.querySelector('#today[empty]')) {
        await new Promise(resolve => setTimeout(resolve, 20));
      }
      await app.updateComplete;
      const today = r.querySelector('#today').getBoundingClientRect();
      const header = r.querySelectorAll('#sections > .section-header')[1]
          .getBoundingClientRect();
      const newTab = r.querySelector('#newTab').getBoundingClientRect();
      return [Math.round(newTab.top - header.bottom), Math.round(today.height)];
    })())";
  base::ListValue room = content::EvalJs(contents, kRoom).ExtractList().Clone();
  ASSERT_EQ(room.size(), 2u);
  EXPECT_LT(room[0].GetInt(), 12);
  EXPECT_EQ(room[1].GetInt(), 0);

  // While something is dragged, it's a drop zone again.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    (async () => {
      const app = document.querySelector('compass-sidebar-app');
      app.linkDragging_ = true;
      await app.updateComplete;
    })())"));
  EXPECT_GE(content::EvalJs(contents, R"(
    Math.round(document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#today').getBoundingClientRect().height))")
                .ExtractInt(),
            32);
}

// The previews a switch slides in line up with the live sidebar, so nothing
// jumps when the live one takes over: their section headers (plain copies of
// the live buttons) were shorter, moving every entry up.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, PreviewsLineUpWithTheSidebar) {
  SpaceList& spaces =
      WindowFeatures::From(browser())->service().space_list();
  spaces.AddSpace(spaces.spaces().size(), u"Second");
  content::WebContents* contents = sidebar_contents();
  constexpr char kHeaderHeights[] = R"(
    (async () => {
      const r = () => document.querySelector('compass-sidebar-app').shadowRoot;
      const preview = () => r().querySelector(
          'compass-sidebar-preview[side=next]')?.shadowRoot;
      while (!preview()?.querySelector('#sections .section-header')) {
        await new Promise(resolve => setTimeout(resolve, 20));
      }
      const heights = root => [...root.querySelectorAll(
          '#sections > .section-header')].map(h => h.offsetHeight);
      return [heights(r().querySelector('#spacePane')), heights(preview())];
    })())";
  base::ListValue result =
      content::EvalJs(contents, kHeaderHeights).ExtractList().Clone();
  ASSERT_EQ(result.size(), 2u);
  // Pinned's and Today's, the same height in both.
  EXPECT_EQ(result[0].GetList().size(), 2u);
  EXPECT_EQ(result[0], result[1]);
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ReplacesNativeTabStrip) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  ASSERT_TRUE(sidebar());
  EXPECT_TRUE(region->Contains(sidebar()));
  EXPECT_TRUE(sidebar()->GetVisible());
  EXPECT_EQ(sidebar()->GetWebContents()->GetVisibleURL(),
            GURL("chrome://compass-sidebar/"));
  // The native strip is never built.
  EXPECT_FALSE(FindDescendant<TabStripView>(browser_view()));
  EXPECT_FALSE(region->GetTabStripView());
  EXPECT_GT(sidebar()->height(), 0);
  // Upstream's New Tab button under the strip is hidden: the page has its own.
  auto* bottom = FindDescendant<VerticalTabStripBottomContainer>(region);
  ASSERT_TRUE(bottom);
  EXPECT_FALSE(bottom->GetVisible());
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, TintCoversTheWholeRegion) {
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
            ToTintColor(*GetSpaceTint(SpaceColor::kBlue)));
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
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, TintFollowsASwipe) {
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
            ToTintColor(*GetSpaceTint(SpaceColor::kGreen)));
  EXPECT_FALSE(tint->IsAnimatingForTesting());
}

// A gradient's colors are the tint's stops, in the region's tint and the
// frame's alike; a swipe blends them stop by stop, and the theme seed stays
// the space's color.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, TintPaintsAGradient) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  auto* tint = FindDescendant<SidebarTint>(region);
  ASSERT_TRUE(tint);
  auto* frame_tint =
      views::AsViewClass<SidebarTint>(browser_view()->children().front());
  ASSERT_TRUE(frame_tint);
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& home = features->space_model();
  home.SetColor(SpaceColor::kBlue);
  home.SetGradient({SpaceColor::kPink});

  const SidebarTint::Stops& stops = tint->GetStopsForTesting();
  EXPECT_EQ(SkColorSetA(stops[0], SK_AlphaOPAQUE),
            ToTintColor(*SpaceColor::kBlue.tint()));
  EXPECT_EQ(SkColorSetA(stops[2], SK_AlphaOPAQUE),
            ToTintColor(*SpaceColor::kPink.tint()));
  // Halfway between them.
  EXPECT_GT(SkColorGetR(stops[1]), SkColorGetR(stops[0]));
  EXPECT_LT(SkColorGetR(stops[1]), SkColorGetR(stops[2]));
  EXPECT_EQ(browser_view()->GetWidget()->user_color_override(),
            SpaceColor::kBlue.tint());
  // The frame's tint paints the same gradient.
  EXPECT_EQ(frame_tint->GetStopsForTesting(), stops);

  // Three colors, then a swipe toward a plain green space.
  home.SetGradient({SpaceColor::kPink, SpaceColor::kYellow});
  EXPECT_EQ(SkColorSetA(tint->GetStopsForTesting()[1], SK_AlphaOPAQUE),
            ToTintColor(*SpaceColor::kPink.tint()));
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  work->SetColor(SpaceColor::kGreen);
  features->SetSpacePreview(work, 1);
  for (SkColor stop : tint->GetStopsForTesting()) {
    EXPECT_EQ(SkColorSetA(stop, SK_AlphaOPAQUE),
            ToTintColor(*SpaceColor::kGreen.tint()));
  }
  features->SetSpacePreview(nullptr, 0);

  // The tint is Arc's: Claudio's #6cbad9 as Arc showed it, #75aec4.
  const SkColor arc = ToTintColor(SkColorSetRGB(108, 186, 217));
  EXPECT_NEAR(SkColorGetR(arc), 117, 3);
  EXPECT_NEAR(SkColorGetG(arc), 174, 3);
  EXPECT_NEAR(SkColorGetB(arc), 196, 3);

  // Intensity sets the tint's opacity; grain blends like the colors.
  home.SetIntensity(1);
  EXPECT_EQ(SkColorGetA(tint->GetStopsForTesting()[0]), 0xffu);
  home.SetIntensity(0.5);
  EXPECT_EQ(SkColorGetA(tint->GetStopsForTesting()[0]),
            base::ClampRound<SkColor>(0.55 * 255));
  // The phone's default: about 34%.
  home.SetIntensity(kDefaultSpaceIntensity);
  EXPECT_EQ(SkColorGetA(tint->GetStopsForTesting()[0]), 0x57u);
  home.SetGrain(0.6);
  EXPECT_FLOAT_EQ(tint->GetGrainForTesting(), 0.6f);
  EXPECT_FLOAT_EQ(frame_tint->GetGrainForTesting(), 0.6f);
  features->SetSpacePreview(work, 0.5);
  EXPECT_FLOAT_EQ(tint->GetGrainForTesting(), 0.3f);
  features->SetSpacePreview(nullptr, 0);

  // Grey drops the gradient.
  home.SetColor(SpaceColor::kGrey);
  EXPECT_TRUE(home.gradient().empty());
  EXPECT_FALSE(tint->GetTintForTesting());
}

// The space color seeds the window's theme: its frame color follows the
// space, not during a swipe, and a grey space keeps the profile's theme.
// A new window has its space's theme from the start: the tint's first
// update runs while the window is built, before the browser has its window.
// Undocked DevTools is a window of its own, not a tab in the space.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, UndockedDevToolsIsNoTab) {
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  const size_t today = space.today_root()->children().size();
  const size_t pinned = space.pinned_root()->children().size();
  DevToolsWindow* devtools =
      DevToolsWindowTesting::OpenDevToolsWindowSync(browser(), false);
  EXPECT_EQ(today, space.today_root()->children().size());
  EXPECT_EQ(pinned, space.pinned_root()->children().size());
  DevToolsWindowTesting::CloseDevToolsWindowSync(devtools);
  EXPECT_EQ(today, space.today_root()->children().size());
}

// A space that stays on this device (docs/design/sync.md, "Spaces that
// don't sync"): its tabs are left out of session sync, the switcher marks it,
// and its menu turns sync back on in one click.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, LocalSpace) {
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  // A page, so the tab is an entry.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>Local</title>")));
  content::WebContents* tab =
      browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_FALSE(IsInUnsyncedSpace(tab));
  space.SetSynced(false);
  EXPECT_TRUE(IsInUnsyncedSpace(tab));

  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      const until = async (f) => {
        while (!f()) await new Promise(r => setTimeout(r, 20));
        return f();
      };
      const button = await until(
          () => root.querySelector('.space[local-only]'));
      if (!button.title.endsWith('On this device only')) return false;
      root.querySelector('#spaceName').dispatchEvent(new MouseEvent(
          'dblclick', {bubbles: true, composed: true}));
      const toggle = await until(() => {
        const t = root.querySelector('#spaceSync');
        return t && !t.hidden ? t : null;
      });
      if (toggle.getAttribute('aria-checked') !== 'false') return false;
      toggle.click();
      return true;
    })())"));
  ASSERT_TRUE(base::test::RunUntil([&] { return space.synced(); }));
  EXPECT_FALSE(IsInUnsyncedSpace(tab));
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      while (root.querySelector('.space[local-only]')) {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, NewWindowHasTheSpaceTheme) {
  SpaceModel& home = WindowFeatures::From(browser())->space_model();
  home.SetColor(SpaceColor::kBlue);
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  ASSERT_EQ(&WindowFeatures::From(second)->space_model(), &home);
  EXPECT_EQ(BrowserView::GetBrowserViewForBrowser(second)
                ->GetWidget()
                ->user_color_override(),
            GetSpaceTint(SpaceColor::kBlue));
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, WindowThemeFollowsTheSpace) {
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

  // A strong tint picks the mode, so text stays readable: light over a light
  // blue, dark over a navy one; the default intensity leaves the system's.
  using ColorMode = ui::ColorProviderKey::ColorMode;
  EXPECT_FALSE(widget->color_mode_override());
  work->SetColor(SpaceColor::FromRGB(108, 186, 217));
  work->SetIntensity(1);
  EXPECT_EQ(widget->color_mode_override(), ColorMode::kLight);
  work->SetColor(SpaceColor::FromRGB(20, 30, 70));
  EXPECT_EQ(widget->color_mode_override(), ColorMode::kDark);
  // The space's own mode sets the frame under the tint; the text still
  // takes whichever mode reads best over it: light over navy in light mode,
  // dark over the light blue in dark mode.
  work->SetAppearance(SpaceAppearance::kLight);
  EXPECT_EQ(widget->color_mode_override(), ColorMode::kDark);
  work->SetColor(SpaceColor::FromRGB(108, 186, 217));
  EXPECT_EQ(widget->color_mode_override(), ColorMode::kLight);
  work->SetAppearance(SpaceAppearance::kDark);
  EXPECT_EQ(widget->color_mode_override(), ColorMode::kLight);
  // A weak tint leaves the mode's own frame and text.
  work->SetIntensity(0.05);
  EXPECT_EQ(widget->color_mode_override(), ColorMode::kDark);
  work->SetIntensity(1);
  work->SetAppearance(SpaceAppearance::kAuto);
  work->SetIntensity(kDefaultSpaceIntensity);
  EXPECT_FALSE(widget->color_mode_override());

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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, CollapsesAndResizes) {
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

// Dragged narrower than the minimum (but not far enough to collapse), the
// sidebar stops at the minimum (docs/design/window-frame.md, "Width").
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ResizeStopsAtTheMinimum) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  views::Widget* widget = browser_view()->GetWidget();
  const int below = tabs::kVerticalTabStripUncollapsedMinWidth - 20;
  ASSERT_GT(below, tabs::kVerticalTabStripCollapseSnapWidth);
  region->OnResize(below - region->width(), /*done_resizing=*/true);
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return region->width() == tabs::kVerticalTabStripUncollapsedMinWidth;
  }));
  EXPECT_TRUE(sidebar()->IsDrawn());
}

// A double click on the resize area puts the default width back.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, DoubleClickResetsTheWidth) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  views::Widget* widget = browser_view()->GetWidget();
  region->OnResize(
      tabs::kVerticalTabStripUncollapsedMaxWidth - region->width(),
      /*done_resizing=*/true);
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return region->width() == tabs::kVerticalTabStripUncollapsedMaxWidth;
  }));

  views::ResizeArea* handle = region->resize_area_for_testing();
  ASSERT_TRUE(handle);
  const gfx::Point point(1, 10);
  // The first click of the pair starts a drag as usual and ends it unmoved.
  ui::MouseEvent first(ui::EventType::kMousePressed, point, point,
                       ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON,
                       ui::EF_LEFT_MOUSE_BUTTON);
  EXPECT_TRUE(handle->OnMousePressed(first));
  ui::MouseEvent second(ui::EventType::kMousePressed, point, point,
                        ui::EventTimeForNow(),
                        ui::EF_LEFT_MOUSE_BUTTON | ui::EF_IS_DOUBLE_CLICK,
                        ui::EF_LEFT_MOUSE_BUTTON);
  // Not handled: no drag starts, so its release reports nothing.
  EXPECT_FALSE(handle->OnMousePressed(second));
  EXPECT_TRUE(base::test::RunUntil([&] {
    widget->LayoutRootViewIfNecessary();
    return region->width() == tabs::kVerticalTabStripDefaultUncollapsedWidth;
  }));
  EXPECT_EQ(region->width(), tabs::kVerticalTabStripDefaultUncollapsedWidth);
}

// Web pages follow the system's light or dark mode, not their space's
// (docs/design/spaces.md, "Theme editor"); the browser's WebUI follows the
// space's.
class PageColorSchemeBrowserTest : public InProcessBrowserTest {
 protected:
  ui::MockOsSettingsProvider os_settings_provider_;
};

IN_PROC_BROWSER_TEST_F(PageColorSchemeBrowserTest, PagesFollowTheSystem) {
  os_settings_provider_.SetPreferredColorScheme(
      ui::NativeTheme::PreferredColorScheme::kLight);
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  space.SetAppearance(SpaceAppearance::kDark);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<p>page</p>")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  auto page_is_dark = [&] {
    return content::EvalJs(contents,
                           "matchMedia('(prefers-color-scheme: dark)').matches")
        .ExtractBool();
  };
  EXPECT_FALSE(page_is_dark());

  os_settings_provider_.SetPreferredColorScheme(
      ui::NativeTheme::PreferredColorScheme::kDark);
  EXPECT_TRUE(base::test::RunUntil(page_is_dark));
  space.SetAppearance(SpaceAppearance::kLight);
  EXPECT_TRUE(page_is_dark());
}

// Closing, the header and the page slide out sideways at their expanded size:
// nothing moves up or down. Measured at the shrinking width, the header took
// its taller collapsed layout halfway and pushed the page down.
// Motions run here; tests step them by hand (never Reset() one that plays).
class CompassSidebarMotionBrowserTest : public CompassSidebarBrowserTest {
 protected:
  CompassSidebarMotionBrowserTest()
      : CompassSidebarBrowserTest(
            gfx::Animation::RichAnimationRenderMode::FORCE_ENABLED) {}
};

IN_PROC_BROWSER_TEST_F(CompassSidebarMotionBrowserTest,
                       SlidesOutWithoutMovingDown) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  views::Widget* widget = browser_view()->GetWidget();
  widget->LayoutRootViewIfNecessary();
  const int expanded_width = region->width();
  const gfx::Rect page = sidebar()->GetBoundsInScreen();
  ASSERT_FALSE(page.IsEmpty());

  auto container = base::MakeRefCounted<gfx::AnimationContainer>();
  auto* controller = BrowserAnimationController::From(browser());
  controller->SetAnimationContainerForTesting(
      TabStripAnimations::kVerticalTabStrip, container.get());
  gfx::AnimationContainerTestApi test_api(container.get());

  region->RequestCollapse(true);
  ASSERT_TRUE(controller->IsAnimating(TabStripAnimations::kVerticalTabStrip));
  bool went_below_snap = false;
  for (int step = 0;
       step < 100 &&
       controller->IsAnimating(TabStripAnimations::kVerticalTabStrip);
       ++step) {
    test_api.IncrementTime(base::Milliseconds(10));
    widget->LayoutRootViewIfNecessary();
    if (!sidebar()->IsDrawn()) {
      continue;
    }
    SCOPED_TRACE(testing::Message() << "region width " << region->width());
    went_below_snap |=
        region->width() < tabs::kVerticalTabStripCollapseSnapWidth;
    const gfx::Rect now = sidebar()->GetBoundsInScreen();
    EXPECT_EQ(now.y(), page.y());
    EXPECT_EQ(now.height(), page.height());
    EXPECT_EQ(now.width(), page.width());
    EXPECT_EQ(now.right() - page.right(), region->width() - expanded_width);
  }
  EXPECT_TRUE(went_below_snap);
  EXPECT_FALSE(controller->IsAnimating(TabStripAnimations::kVerticalTabStrip));
  widget->LayoutRootViewIfNecessary();
  EXPECT_EQ(region->width(), kCollapsedSidebarWidth);
  EXPECT_FALSE(sidebar()->IsDrawn());
}

// The page sits in a round-cornered card inside a frame of the space's color,
// which drags the window. Collapsed, the sidebar is the frame's leading edge
// and the caption buttons hide.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, WindowFrame) {
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ListsTabs) {
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ClickingATabActivatesIt) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_EQ(model->active_index(), 1);
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kWaitForTabRows, 2)));

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelectorAll('.tab')[0].click();)"));

  EXPECT_TRUE(base::test::RunUntil([&] { return model->active_index() == 0; }));
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
                       NewTabButtonOpensTheCommandBar) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kWaitForTabRows, 1)));

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#newTab').click();)"));

  CommandBar* bar = CommandBar::From(browser());
  ASSERT_TRUE(bar);
  EXPECT_TRUE(base::test::RunUntil([&] { return bar->IsShowing(); }));
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
}

// Waits until the sidebar's tab rows have the titles |titles|, in order.
constexpr char kWaitForTitles[] = R"(
  (async (titles) => {
    const current = () => [...(document.querySelector('compass-sidebar-app')
        ?.shadowRoot?.querySelectorAll('.tab') ?? [])].map(r => r.title);
    while (JSON.stringify(current()) !== JSON.stringify(titles)) {
      await new Promise(r => setTimeout(r, 20));
    }
    return true;
  })($1))";

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
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
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
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

// A press that lands while two fingers scroll moves like a drag; the
// sidebar leaves it to the scroll rather than picking up the row under the
// pointer. A drag once the scroll has stopped starts as usual.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, NoDragMidScroll) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(contents,
                            content::JsReplace(kWaitForTitles,
                                               base::ListValue().Append("One"))));

  constexpr char kDragstart[] = R"(
    (async (wheel) => {
      const app = document.querySelector('compass-sidebar-app');
      const row = [...app.shadowRoot.querySelectorAll('.row')]
          .find(r => r.title === 'One' || r.textContent.trim() === 'One');
      if (wheel) {
        row.dispatchEvent(new WheelEvent(
            'wheel', {bubbles: true, composed: true, deltaY: 4}));
      } else {
        await new Promise(r => setTimeout(r, 500));
      }
      const init = {bubbles: true, cancelable: true, composed: true,
                    dataTransfer: new DataTransfer()};
      const started = row.dispatchEvent(new DragEvent('dragstart', init));
      row.dispatchEvent(new DragEvent('dragend', init));
      return started;
    })($1))";
  EXPECT_EQ(false,
            content::EvalJs(contents, content::JsReplace(kDragstart, true)));
  EXPECT_EQ(true,
            content::EvalJs(contents, content::JsReplace(kDragstart, false)));
}

// A loaded pinned tab's − closes the tab at the first click; the entry
// stays. An unloaded entry's × removes it, at the second click
// (docs/design/sidebar.md, "Closing pinned entries").
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, PinnedCloseUnloadsAtOnce) {
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
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& model = features->space_model();
  SidebarNode* two = model.today_root()->children()[1].get();
  model.Move(two, model.pinned_root(), 0);
  ASSERT_TRUE(features->registry().GetTab(two));
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      const close = () => root.querySelector('#pinned .close');
      while (!close()) await new Promise(r => setTimeout(r, 20));
      return close().textContent === '\u2212' &&
          root.querySelector('#today .close').textContent === '\u00d7';
    })())"));
  constexpr char kClickPinnedClose[] = R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      const close = () => root.querySelector('#pinned .close');
      while (!close()) await new Promise(r => setTimeout(r, 20));
      close().click();
      return true;
    })())";

  ASSERT_EQ(true, content::EvalJs(contents, kClickPinnedClose));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !features->registry().GetTab(two); }));
  ASSERT_EQ(model.pinned_root()->children().size(), 1u);

  // Unloaded, the first click only arms it.
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      while (root.querySelector('#pinned .tab:not([unloaded])')) {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));
  ASSERT_EQ(true, content::EvalJs(contents, kClickPinnedClose));
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (() => {
      const close = document.querySelector('compass-sidebar-app').shadowRoot
          .querySelector('#pinned .close');
      return close.hasAttribute('armed') && close.textContent === '\u00d7';
    })())"));
  EXPECT_EQ(model.pinned_root()->children().size(), 1u);
  ASSERT_EQ(true, content::EvalJs(contents, kClickPinnedClose));
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return model.pinned_root()->children().empty(); }));
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, DragToPinnedPinsATab) {
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
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
      document.querySelector('compass-sidebar-app').shadowRoot
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
               document.querySelector('compass-sidebar-app').shadowRoot
                   .querySelector('.tile').hasAttribute('unloaded'))")
                   .ExtractBool();
  }));
  ASSERT_TRUE(content::ExecJs(contents, R"(
      document.querySelector('compass-sidebar-app').shadowRoot
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
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, DroppingALinkOpensIt) {
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
  EXPECT_EQ(contents->GetLastCommittedURL(), GURL("chrome://compass-sidebar/"));
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, SwitchesSpaces) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* one = browser()->tab_strip_model()->GetWebContentsAt(0);
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  content::WebContents* contents = sidebar_contents();

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#addSpace').click();
  )"));

  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  EXPECT_EQ(features->space_model().name(), u"Space 2");
  EXPECT_NE(browser()->tab_strip_model()->GetActiveWebContents(), one);
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
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
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('.space').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() == home; }));
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), one);

  // And again.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelectorAll('.space')[1].click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
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
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
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
    document.querySelector('compass-sidebar-app').shadowRoot
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

// Spike, docs/design/profiles-in-place.md: a space of another profile shows
// in the same native window (the BrowserViews are exchanged), and back.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
                       AnotherProfilesSpaceKeepsTheNativeWindow) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* one = browser()->tab_strip_model()->GetWebContentsAt(0);
  Profile* profile = browser()->GetProfile();
  const gfx::NativeWindow native = browser()->GetWindow()->GetNativeWindow();
  views::Widget* widget = views::Widget::GetWidgetForNativeWindow(native);
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  SpaceDirectory::Get()->List(&other);
  SpaceModel& other_home = SpaceService::Get(&other)->space_list().active_space();

  SetMaxTreeWaitForTesting(base::Seconds(10));
  BrowserWindowInterface* swapped =
      SwitchWindowToSpace(browser(), &other, other_home);
  ASSERT_TRUE(swapped);
  EXPECT_EQ(swapped->GetProfile(), &other);
  // Its view takes the native window once its sidebar has the tree, so the
  // sidebar never shows empty.
  bool had_tree = false;
  ASSERT_TRUE(base::test::RunUntil([&] {
    if (swapped->GetWindow()->GetNativeWindow() != native) {
      return false;
    }
    content::WebContents* contents =
        FindDescendant<CompassSidebarView>(
            BrowserView::GetBrowserViewForBrowser(swapped))
            ->GetWebContents();
    contents->GetWebUI()->GetController()->GetAs<CompassSidebarUI>()
        ->RunWhenTreeShown(base::BindLambdaForTesting([&] { had_tree = true; }));
    return true;
  }));
  EXPECT_TRUE(had_tree);
  EXPECT_EQ(BrowserView::GetBrowserViewForNativeWindow(native),
            BrowserView::GetBrowserViewForBrowser(swapped));
  EXPECT_EQ(static_cast<Profile*>(
                widget->GetNativeWindowProperty(Profile::kProfileKey)),
            &other);
  EXPECT_EQ(widget->widget_delegate(),
            static_cast<views::WidgetDelegate*>(
                BrowserView::GetBrowserViewForBrowser(swapped)));
  // The old window empties into its parking window and closes.
  ASSERT_TRUE(base::test::RunUntil([&] {
    int normal = 0;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          normal += window->GetType() == BrowserWindowInterface::TYPE_NORMAL;
          return true;
        });
    return normal == 1;
  }));
  SetBrowser(swapped);
  BrowserWindowInterface* parking =
      SpaceService::Get(profile)->parking_window();
  ASSERT_TRUE(parking);
  EXPECT_EQ(parking->GetTabStripModel()->GetWebContentsAt(0), one);

  // The new profile's window works in it: navigating, accelerators.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      swapped, GURL("data:text/html,<title>Two</title>")));
  EXPECT_EQ(swapped->GetTabStripModel()->GetActiveWebContents()
                ->GetBrowserContext(),
            &other);
  widget->LayoutRootViewIfNecessary();
  EXPECT_FALSE(BrowserView::GetBrowserViewForBrowser(swapped)
                   ->GetBoundsInScreen()
                   .IsEmpty());

  // And back, by shortcut: the accelerators moved with the view.
  const std::vector<SpaceDirectory::Entry> spaces =
      GetSwitchableSpaces(swapped);
  ASSERT_EQ(spaces.size(), 2u);
  const int index = spaces[0].profile == profile ? 0 : 1;
  ASSERT_TRUE(PressSpaceShortcut(
      BrowserView::GetBrowserViewForBrowser(swapped), index));
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
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return back->GetWindow()->GetNativeWindow() == native; }));
  EXPECT_EQ(back->GetTabStripModel()->GetActiveWebContents(), one);
  ASSERT_TRUE(base::test::RunUntil([&] {
    int normal = 0;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          normal += window->GetType() == BrowserWindowInterface::TYPE_NORMAL;
          return true;
        });
    return normal == 1;
  }));
}

// Switching between two profiles' spaces several times in a row, each switch
// as soon as the window shows the last one, before the old view has left. The
// other profile's tab has an infobar, which shows in a window built hidden and
// must leave that window's focus manager when it closes.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
                       SwitchingProfilesBackAndForth) {
  SetMaxTreeWaitForTesting(base::Seconds(10));
  Profile* profile = browser()->GetProfile();
  const gfx::NativeWindow native = browser()->GetWindow()->GetNativeWindow();
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  SpaceDirectory::Get()->List(&other);
  SpaceModel& home = SpaceService::Get(profile)->space_list().active_space();
  SpaceModel& other_home = SpaceService::Get(&other)->space_list().active_space();

  BrowserWindowInterface* shown = browser();
  for (int i = 1; i <= 6; ++i) {
    BrowserWindowInterface* next =
        i % 2 ? SwitchWindowToSpace(shown, &other, other_home)
              : SwitchWindowToSpace(shown, profile, home);
    ASSERT_TRUE(next) << i;
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return next->GetWindow()->GetNativeWindow() == native; }))
        << i;
    shown = next;
    SetBrowser(shown);
    if (i == 1) {
      CreateSimpleAlertInfoBar(
          infobars::ContentInfoBarManager::FromWebContents(
              shown->GetTabStripModel()->GetActiveWebContents()),
          infobars::InfoBarDelegate::TEST_INFOBAR, nullptr, u"Infobar",
          /*auto_expire=*/false, /*should_animate=*/false);
    }
  }
  ASSERT_TRUE(base::test::RunUntil([&] {
    int normal = 0;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          normal += window->GetType() == BrowserWindowInterface::TYPE_NORMAL;
          return true;
        });
    return normal == 1;
  }));
  EXPECT_EQ(shown->GetProfile(), profile);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      shown, GURL("data:text/html,<title>After</title>")));
}

// Assigning the window's space to another profile from the space menu, as in
// Arc: the window swaps to that profile and reloads the page it showed.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, SpaceMenuAssignsAProfile) {
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
          const root = document.querySelector('compass-sidebar-app').shadowRoot;
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


// The sidebar is the browser's UI, not a page: pinches and smart zoom never
// reach it, it stays at 100% whatever the default page zoom, and its text
// can't be selected (names being edited can).
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, BehavesLikeBrowserUI) {
  content::WebContents* contents = sidebar_contents();
  content::WebContentsDelegate* delegate = contents->GetDelegate();
  ASSERT_TRUE(delegate);
  for (blink::WebInputEvent::Type type :
       {blink::WebInputEvent::Type::kGesturePinchBegin,
        blink::WebInputEvent::Type::kGesturePinchUpdate,
        blink::WebInputEvent::Type::kGesturePinchEnd,
        blink::WebInputEvent::Type::kGestureDoubleTap}) {
    blink::WebGestureEvent event(type, blink::WebInputEvent::kNoModifiers,
                                 base::TimeTicks::Now(),
                                 blink::WebGestureDevice::kTouchpad);
    EXPECT_TRUE(delegate->PreHandleGestureEvent(contents, event));
  }
  // Scrolling still gets through.
  blink::WebGestureEvent scroll(
      blink::WebInputEvent::Type::kGestureScrollUpdate,
      blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now(),
      blink::WebGestureDevice::kTouchpad);
  EXPECT_FALSE(delegate->PreHandleGestureEvent(contents, scroll));

  browser()->GetProfile()->GetZoomLevelPrefs()->SetDefaultZoomLevelPref(
      blink::ZoomFactorToZoomLevel(1.25));
  EXPECT_EQ(content::HostZoomMap::GetZoomLevel(contents), 0.0);

  EXPECT_EQ(content::EvalJs(contents, R"(
      getComputedStyle(document.querySelector('compass-sidebar-app'))
          .userSelect)"),
            "none");
  EXPECT_EQ(content::EvalJs(contents, R"(
      getComputedStyle(document.body).overscrollBehaviorX)"),
            "none");
}

// A trackpad scroll that starts sideways is a swipe: the browser takes it
// from the page (so vertical scrolls never wait on the page's script) and
// tells the page, which pulls in the next space. Vertical ones scroll.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, SwipesAreTheBrowsers) {
  content::WebContents* contents = sidebar_contents();
  content::WebContentsDelegate* delegate = contents->GetDelegate();
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  using Type = blink::WebInputEvent::Type;
  using Phase = blink::WebGestureEvent::InertialPhaseState;
  auto gesture = [&](Type type, float dx, float dy,
                     Phase phase = Phase::kNonMomentum) {
    blink::WebGestureEvent event(type, blink::WebInputEvent::kNoModifiers,
                                 base::TimeTicks::Now(),
                                 blink::WebGestureDevice::kTouchpad);
    if (type == Type::kGestureScrollBegin) {
      event.data.scroll_begin.delta_x_hint = dx;
      event.data.scroll_begin.delta_y_hint = dy;
      event.data.scroll_begin.inertial_phase = phase;
    } else if (type == Type::kGestureScrollUpdate) {
      event.data.scroll_update.delta_x = dx;
      event.data.scroll_update.delta_y = dy;
      event.data.scroll_update.inertial_phase = phase;
    } else if (type == Type::kGestureScrollEnd) {
      event.data.scroll_end.inertial_phase = phase;
    }
    return delegate->PreHandleGestureEvent(contents, event);
  };

  // Vertical: the page scrolls it.
  EXPECT_FALSE(gesture(Type::kGestureScrollBegin, 0, -4));
  EXPECT_FALSE(gesture(Type::kGestureScrollUpdate, 1, -20));
  EXPECT_FALSE(gesture(Type::kGestureScrollEnd, 0, 0));
  EXPECT_EQ(&features->space_model(), home);

  // Fingers to the left, past half the sidebar: the next space. The page
  // gets the scroll's begin and end, so the input router knows it started
  // and ends it (SwipesEndThroughTheWheelQueue); only its updates are taken.
  EXPECT_FALSE(gesture(Type::kGestureScrollBegin, -6, 1));
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(gesture(Type::kGestureScrollUpdate, -80, 0));
  }
  EXPECT_FALSE(gesture(Type::kGestureScrollEnd, 0, 0));
  EXPECT_TRUE(
      base::test::RunUntil([&] { return &features->space_model() == work; }));

  // Its momentum's updates are swallowed; the next scroll isn't.
  EXPECT_FALSE(gesture(Type::kGestureScrollBegin, -30, 0, Phase::kMomentum));
  EXPECT_TRUE(gesture(Type::kGestureScrollUpdate, -30, 0, Phase::kMomentum));
  EXPECT_FALSE(gesture(Type::kGestureScrollEnd, 0, 0, Phase::kMomentum));
  EXPECT_FALSE(gesture(Type::kGestureScrollBegin, 0, -4));
  EXPECT_FALSE(gesture(Type::kGestureScrollUpdate, 0, -20));
}

// A swipe as the trackpad sends it: wheel events with phases, which the
// input router turns into a scroll's begin, updates and end. Taking the
// begin from the page left the router thinking no scroll had started: it
// sent a begin before every update, which restarted the swipe, and never an
// end, so the page stayed mid-swipe and its entries wouldn't scroll.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
                       SwipesEndThroughTheWheelQueue) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(content::ExecJs(contents, R"(
    (async () => {
      const app = () => document.querySelector('compass-sidebar-app');
      while ((app()?.spaces_?.length ?? 0) < 2) {
        await new Promise(resolve => setTimeout(resolve, 20));
      }
    })())"));
  content::RenderWidgetHost* host =
      contents->GetRenderWidgetHostView()->GetRenderWidgetHost();
  const gfx::PointF point(sidebar()->width() / 2, sidebar()->height() / 2);
  using Wheel = blink::WebMouseWheelEvent;
  // One at a time: queued ones would coalesce into a single big update.
  auto wheel = [&](float dx, Wheel::Phase phase,
                   Wheel::Phase momentum = Wheel::kPhaseNone) {
    Wheel event(blink::WebInputEvent::Type::kMouseWheel,
                blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
    event.SetPositionInWidget(point);
    event.SetPositionInScreen(point);
    event.delta_x = dx;
    event.phase = phase;
    event.momentum_phase = momentum;
    event.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
    event.event_action = Wheel::EventAction::kScroll;
    content::InputEventAckWaiter waiter(
        host, blink::WebInputEvent::Type::kMouseWheel);
    host->ForwardWheelEvent(event);
    waiter.Wait();
  };
  // Whether the page is mid-swipe or its entries can't scroll.
  constexpr char kStuck[] = R"(
    (async () => {
      const app = document.querySelector('compass-sidebar-app');
      // A swipe that let go settles first.
      await new Promise(resolve => setTimeout(resolve, 600));
      return !!app.swipe_ ||
          app.shadowRoot.querySelector('#stage').hasAttribute('sliding');
    })())";

  // The entries follow the fingers past half the sidebar and back: letting
  // go short of a quarter stays.
  const int width = content::EvalJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#stage').clientWidth)")
                        .ExtractInt();
  ASSERT_GT(width, 80);
  wheel(-4, Wheel::kPhaseBegan);
  for (int pulled = 4; pulled < width * 0.6; pulled += 8) {
    wheel(-8, Wheel::kPhaseChanged);
  }
  EXPECT_EQ(&features->space_model(), home);
  for (int i = 0; i < width * 0.5 / 8; ++i) {
    wheel(8, Wheel::kPhaseChanged);
  }
  wheel(0, Wheel::kPhaseEnded);
  EXPECT_EQ(false, content::EvalJs(contents, kStuck));
  EXPECT_EQ(&features->space_model(), home);

  // Small steps, as a slow swipe sends, add up past half the sidebar; the
  // switch comes as the fingers lift.
  wheel(-4, Wheel::kPhaseBegan);
  for (int pulled = 4; pulled < width * 0.6; pulled += 8) {
    wheel(-8, Wheel::kPhaseChanged);
  }
  EXPECT_EQ(&features->space_model(), home);
  wheel(0, Wheel::kPhaseEnded);
  EXPECT_TRUE(
      base::test::RunUntil([&] { return &features->space_model() == work; }));
  // Its momentum doesn't start another swipe.
  wheel(-8, Wheel::kPhaseNone, Wheel::kPhaseBegan);
  for (int i = 0; i < 10; ++i) {
    wheel(-6, Wheel::kPhaseNone, Wheel::kPhaseChanged);
  }
  wheel(0, Wheel::kPhaseNone, Wheel::kPhaseEnded);
  EXPECT_EQ(false, content::EvalJs(contents, kStuck));

  // A short pull back toward the first space lets go and settles.
  wheel(4, Wheel::kPhaseBegan);
  for (int i = 0; i < 3; ++i) {
    wheel(8, Wheel::kPhaseChanged);
  }
  wheel(0, Wheel::kPhaseEnded);
  EXPECT_EQ(false, content::EvalJs(contents, kStuck));
  EXPECT_EQ(&features->space_model(), work);
}

// With spaces of two profiles there's no profile picker at startup, and the
// window shows the switcher's first space in its profile, though the profile
// used last is the other one (docs/design/spaces.md, "Startup with several
// profiles").
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
                       PRE_StartupShowsTheFirstSpace) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  SpaceService* service = SpaceService::Get(&other);
  ASSERT_TRUE(base::test::RunUntil([&] { return service->loaded(); }));
  SpaceDirectory* directory = SpaceDirectory::Get();
  ASSERT_TRUE(base::test::RunUntil([&] { return directory->IsLoaded(); }));
  directory->List(&other);
  SpaceModel* first = service->space_list().AddSpace(0, u"First");
  directory->MoveSpace(*first, 0);
  ASSERT_EQ(directory->GetSpaces()[0].space, first);
  service->store()->Flush();
  directory->Flush();
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, StartupShowsTheFirstSpace) {
  BrowserWindowInterface* shown = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    shown = nullptr;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          WindowFeatures* features = WindowFeatures::From(window);
          if (window->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
              features && !features->is_parking_window() &&
              !features->swapped_away() &&
              features->space_model().name() == u"First") {
            shown = window;
          }
          return !shown;
        });
    return shown != nullptr;
  }));
  // Not the default profile, which the test harness opened first.
  EXPECT_NE(shown->GetProfile()->GetBaseName(),
            base::FilePath::FromASCII(chrome::kInitialProfile));
  // The window it swapped from closed.
  ASSERT_TRUE(base::test::RunUntil([&] {
    int normal = 0;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          WindowFeatures* features = WindowFeatures::From(window);
          normal += window->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
                    features && !features->is_parking_window();
          return true;
        });
    return normal == 1;
  }));
  SetBrowser(shown);
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest,
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

// Drags the space button titled $1 onto the one titled $2, at fraction $3 of
// its width, using synthetic drag events.
constexpr char kDragSpace[] = R"(
  (async (from, to, fraction) => {
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
    const until = async (f) => {
      while (!f()) await new Promise(r => setTimeout(r, 20));
      return f();
    };
    // No template literals: JsReplace() takes their $ for a placeholder.
    const button = (title) => [...root.querySelectorAll('.space')]
        .find(b => b.title === title);
    const source = await until(() => button(from));
    const target = await until(() => button(to));
    const rect = target.getBoundingClientRect();
    const init = {
      bubbles: true, cancelable: true, composed: true,
      dataTransfer: new DataTransfer(),
      clientX: rect.left + rect.width * fraction,
      clientY: rect.top + rect.height / 2,
    };
    source.dispatchEvent(new DragEvent('dragstart', init));
    target.dispatchEvent(new DragEvent('dragover', init));
    const marked = target.getAttribute('reorder');
    target.dispatchEvent(new DragEvent('drop', init));
    source.dispatchEvent(new DragEvent('dragend', init));
    return marked ?? 'none';
  })($1, $2, $3))";

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, DraggingASpaceReordersThem) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceList& list = features->service().space_list();
  SpaceModel* home = &features->space_model();
  SpaceModel* work = list.AddSpace(1, u"Work");
  SpaceModel* play = list.AddSpace(2, u"Play");
  auto order = [&] {
    std::vector<SpaceModel*> spaces;
    for (const SpaceDirectory::Entry& entry :
         GetSwitchableSpaces(browser())) {
      spaces.push_back(entry.space);
    }
    return spaces;
  };
  ASSERT_THAT(order(), testing::ElementsAre(home, work, play));

  // Before Home: first.
  EXPECT_EQ("before", content::EvalJs(sidebar_contents(),
                                      content::JsReplace(kDragSpace, "Play",
                                                         "Home", 0.25)));
  ASSERT_TRUE(base::test::RunUntil([&] { return order()[0] == play; }));
  EXPECT_THAT(order(), testing::ElementsAre(play, home, work));

  // Right after itself: nothing to show, nothing moves.
  EXPECT_EQ("none", content::EvalJs(sidebar_contents(),
                                    content::JsReplace(kDragSpace, "Home",
                                                       "Work", 0.25)));
  EXPECT_THAT(order(), testing::ElementsAre(play, home, work));

  // Ctrl+1 follows the new order.
  EXPECT_TRUE(PressSpaceShortcut(browser_view(), 0));
  EXPECT_EQ(&features->space_model(), play);
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, SpaceMenuRenamesSetsIconsDeletes) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#addSpace').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  SpaceModel* added = &features->space_model();

  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
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
      return true;
    })())"));
  ASSERT_TRUE(base::test::RunUntil([&] { return added->name() == u"Work"; }));

  // A symbol, then an emoji (only its first character is kept), as the
  // switcher's button shows them.
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (() => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      root.querySelector('.icon-choice[title="Travel"]').click();
      return true;
    })())"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return added->icon() == *SpaceIcon::Symbol("travel"); }));
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      while (!root.querySelectorAll('.space')[1].querySelector('svg path')) {
        await new Promise(r => setTimeout(r, 20));
      }
      const input = root.querySelector('#emojiInput');
      input.value = '🚀✨';
      input.dispatchEvent(new Event('input', {bubbles: true}));
      return input.value === '🚀';
    })())"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return added->icon() == *SpaceIcon::Emoji("🚀"); }));
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      const emoji = () =>
          root.querySelectorAll('.space')[1].querySelector('.space-emoji');
      while (!emoji()) await new Promise(r => setTimeout(r, 20));
      return emoji().textContent === '🚀';
    })())"));

  // Delete wants a second click. The window goes back to Home.
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
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

// The space menu's "Edit theme" opens the editor in its place: the palette
// (colors, then gradients and any color), the pad's dots and their count,
// the intensity wave, the grain dial and the mode; Esc goes back.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ThemeEditorEditsTheSpace) {
  SpaceModel& space = WindowFeatures::From(browser())->space_model();
  content::WebContents* contents = sidebar_contents();
  // |action| runs in the editor's shadow root, |editor|.
  auto in_editor = [&](std::string_view action) {
    return content::EvalJs(contents, base::StrCat({R"(
      (async () => {
        const root = document.querySelector('compass-sidebar-app').shadowRoot;
        const until = async (f) => {
          while (!f()) await new Promise(r => setTimeout(r, 20));
          return f();
        };
        const host = await until(
            () => root.querySelector('compass-theme-editor'));
        await host.updateComplete;
        const editor = host.shadowRoot;
        )", action, R"(
        return true;
      })())"}));
  };
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      root.querySelector('.space').dispatchEvent(new MouseEvent(
          'contextmenu', {bubbles: true, cancelable: true, composed: true}));
      while (!root.querySelector('#editTheme')) {
        await new Promise(r => setTimeout(r, 20));
      }
      root.querySelector('#editTheme').click();
      return true;
    })())"));

  ASSERT_EQ(true, in_editor("editor.querySelector('.swatch[title=Blue]').click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return space.color() == SpaceColor::kBlue; }));

  // + adds a dot, its keys turn it, − takes it away.
  ASSERT_EQ(true, in_editor("editor.querySelector('#addColor').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] { return space.gradient().size() == 1; }));
  const SpaceColor added = space.gradient()[0];
  ASSERT_EQ(true, in_editor(R"(
      const dot = await until(
          () => editor.querySelector('.dot[data-index="1"]'));
      dot.dispatchEvent(new KeyboardEvent('keydown', {key: 'ArrowRight'}));)"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return space.gradient().size() == 1 && space.gradient()[0] != added;
  }));
  EXPECT_EQ(space.color(), SpaceColor::kBlue);
  ASSERT_EQ(true, in_editor("editor.querySelector('#removeColor').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] { return space.gradient().empty(); }));

  // Dragging the big dot takes the gradient along.
  space.SetGradient({SpaceColor::kPink});
  ASSERT_EQ(true, in_editor(R"(
      await until(() => editor.querySelectorAll('.dot').length === 2);
      editor.querySelector('.dot[main]').dispatchEvent(
          new KeyboardEvent('keydown', {key: 'ArrowLeft'}));)"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return space.color() != SpaceColor::kBlue &&
           space.gradient() != std::vector<SpaceColor>{SpaceColor::kPink};
  }));

  // The gradients' page, and any color for the main one.
  ASSERT_EQ(true, in_editor(R"(
      editor.querySelector('.page[title=Gradients]').click();
      await host.updateComplete;
      editor.querySelector('.swatch[title=Ember]').click();)"));
  ASSERT_TRUE(base::test::RunUntil([&] { return space.gradient().size() == 2; }));
  EXPECT_EQ(space.color(), SpaceColor::kRed);
  ASSERT_EQ(true, in_editor(R"(
      const input = editor.querySelector('#customColor input');
      input.value = '#12ab0f';
      input.dispatchEvent(new Event('input', {bubbles: true}));)"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return space.color() == SpaceColor::FromRGB(0x12, 0xab, 0x0f);
  }));
  EXPECT_EQ(space.gradient().size(), 2u);

  // Intensity and grain by their keys; the dark mode.
  ASSERT_EQ(true, in_editor(R"(
      editor.querySelector('#wave').dispatchEvent(
          new KeyboardEvent('keydown', {key: 'End'}));
      editor.querySelector('#dial').dispatchEvent(
          new KeyboardEvent('keydown', {key: 'PageUp'}));
      editor.querySelector('.mode[title=Dark]').click();)"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return space.intensity() == 1 && space.grain() > 0.09 &&
           space.appearance() == SpaceAppearance::kDark;
  }));
  // Its text reads best over the tint, which at full intensity hides the
  // frame (GetTintTextMode()).
  EXPECT_EQ(browser_view()->GetWidget()->color_mode_override(),
            GetTintTextMode(space, SK_ColorBLACK));

  // Esc goes back to the menu.
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      root.querySelector('#spaceMenu').dispatchEvent(
          new KeyboardEvent('keydown', {key: 'Escape', bubbles: true}));
      while (!root.querySelector('#spaceNameInput')) {
        await new Promise(r => setTimeout(r, 20));
      }
      return !root.querySelector('compass-theme-editor');
    })())"));
  // Automatic: at full intensity the tint picks the text's mode, at the
  // default the system does.
  space.SetAppearance(SpaceAppearance::kAuto);
  EXPECT_EQ(browser_view()->GetWidget()->color_mode_override(),
            GetTintTextMode(space, SK_ColorBLACK));
  space.SetIntensity(kDefaultSpaceIntensity);
  EXPECT_FALSE(browser_view()->GetWidget()->color_mode_override());
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, DragIntoAFolder) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(kWaitForTitles,
                                             base::ListValue().Append("One"))));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
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
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, FolderCollapseIsShared) {
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  SidebarNode* folder = model.AddFolder(model.pinned_root(), 0, u"Work");
  model.AddTab(folder, 0, GURL("https://a.test/"), u"Inside");
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true, content::EvalJs(
                      contents, content::JsReplace(kWaitForFolder, false)));

  // Clicking the folder collapses it in the model, which saves it.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('.folder-row').click();)"));
  ASSERT_TRUE(base::test::RunUntil([&] { return folder->collapsed(); }));
  EXPECT_EQ(true, content::EvalJs(contents,
                                  content::JsReplace(kWaitForFolder, true)));

  // Another window shows it collapsed too.
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  content::WebContents* second_contents =
      FindDescendant<CompassSidebarView>(
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
    const target = document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector($1);
    const rect = target.getBoundingClientRect();
    return !target.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, composed: true, cancelable: true, button: 2,
      clientX: rect.left + rect.width / 2, clientY: rect.top + rect.height / 2,
    }));
  })())";

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ContextMenus) {
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
    Math.round(document.querySelector('compass-sidebar-app').shadowRoot
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
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#newFolder').click();)"));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
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
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      while (!root.activeElement?.classList.contains('folder-title-input')) {
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

// Clear in Today's header closes every Today tab; the window stays.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ClearClosesTodaysTabs) {
  content::WebContents* contents = sidebar_contents();
  OpenThreeTabs(browser(), contents);
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#clearToday').click();)"));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil([&] {
    for (int i = 0; i < browser()->tab_strip_model()->count(); ++i) {
      if (browser()->tab_strip_model()->GetWebContentsAt(i)->GetVisibleURL()
              .SchemeIs("data")) {
        return false;
      }
    }
    return true;
  }));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return model.today_root()->children().empty() ||
           browser()->tab_strip_model()->count() == 1;
  }));
  EXPECT_FALSE(browser()->tab_strip_model()->empty());
  for (const auto& entry : model.today_root()->children()) {
    EXPECT_FALSE(entry->url().SchemeIs("data"));
  }
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, FoldersInToday) {
  content::WebContents* contents = sidebar_contents();
  OpenThreeTabs(browser(), contents);
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('compass-sidebar-app').shadowRoot
        .querySelector('#newTodayFolder').click();)"));

  // Into the folder, at the top of Today: its tab leads the strip.
  ASSERT_EQ(true, content::EvalJs(contents,
                                  content::JsReplace(kDrag, "Three",
                                                     "#today .folder-row",
                                                     0.5)));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return model.today_root()->children().size() == 3u &&
           model.today_root()->children()[0]->children().size() == 1u;
  }));
  const SidebarNode* folder = model.today_root()->children()[0].get();
  EXPECT_EQ(folder->type(), SidebarNode::Type::kFolder);
  EXPECT_EQ(folder->children()[0]->title(), u"Three");
  ASSERT_TRUE(base::test::RunUntil([&] {
    return browser()->tab_strip_model()->GetWebContentsAt(0)->GetTitle() ==
           u"Three";
  }));
  // Still live, still the active tab.
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents()->GetTitle(),
            u"Three");
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, SplitsShowSideBySide) {
  content::WebContents* contents = sidebar_contents();
  OpenThreeTabs(browser(), contents);
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  MultiContentsView* view =
      BrowserView::GetBrowserViewForBrowser(browser())->multi_contents_view();
  ASSERT_FALSE(view->IsInSplitView());

  // One and Three, the active tab: the window shows both, and the sidebar
  // one row for them.
  model.Split(model.today_root()->children()[0].get(),
              model.today_root()->children()[2].get());
  ASSERT_TRUE(base::test::RunUntil([&] { return view->IsInSplitView(); }));
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      while (root.querySelectorAll('#today .split .pane').length !== 2) {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));

  // Separated, it's one page again.
  model.Unsplit(model.today_root()->children()[0].get());
  ASSERT_TRUE(base::test::RunUntil([&] { return !view->IsInSplitView(); }));
  EXPECT_EQ(model.today_root()->children().size(), 3u);
}

// Up to four panes, in a row (docs/design/sidebar.md, "Split tabs").
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, SplitsOfFourShowSideBySide) {
  content::WebContents* contents = sidebar_contents();
  OpenThreeTabs(browser(), contents);
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Four</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return model.today_root()->children().size() == 4u; }));
  MultiContentsView* view =
      BrowserView::GetBrowserViewForBrowser(browser())->multi_contents_view();
  // The panes that show, in order.
  auto shown = [&] {
    std::vector<ContentsContainerView*> panes;
    for (ContentsContainerView* pane : view->contents_container_views()) {
      if (pane->GetVisible() && pane->contents_view()->web_contents()) {
        panes.push_back(pane);
      }
    }
    std::ranges::sort(panes, {}, [](ContentsContainerView* pane) {
      return pane->GetBoundsInScreen().x();
    });
    return panes;
  };

  SidebarNode* split = model.Split(model.today_root()->children()[0].get(),
                                   model.today_root()->children()[1].get());
  model.Split(split, model.today_root()->children()[1].get());
  model.Split(split, model.today_root()->children()[1].get());
  ASSERT_EQ(split->children().size(), 4u);
  ASSERT_TRUE(base::test::RunUntil([&] { return shown().size() == 4u; }));
  EXPECT_TRUE(view->IsInSplitView());
  // Side by side, each a fair share of the width, none over another.
  views::test::RunScheduledLayout(
      BrowserView::GetBrowserViewForBrowser(browser()));
  std::vector<ContentsContainerView*> panes = shown();
  for (size_t i = 0; i < panes.size(); ++i) {
    EXPECT_GT(panes[i]->width(), view->width() / 8);
    if (i > 0) {
      EXPECT_LE(panes[i - 1]->GetBoundsInScreen().right(),
                panes[i]->GetBoundsInScreen().x());
    }
  }
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
      while (root.querySelectorAll('#today .split .pane').length !== 4) {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));

  // A pane closes: three are left, still split.
  const int closing = browser()->tab_strip_model()->GetIndexOfWebContents(
      panes[1]->contents_view()->web_contents());
  browser()->tab_strip_model()->CloseWebContentsAt(closing, CLOSE_NONE);
  ASSERT_TRUE(base::test::RunUntil([&] { return shown().size() == 3u; }));
  EXPECT_EQ(split->children().size(), 3u);

  // Separated, it's one page again.
  model.Unsplit(split);
  ASSERT_TRUE(base::test::RunUntil([&] { return !view->IsInSplitView(); }));
  EXPECT_EQ(shown().size(), 1u);
}

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, DropOnATabSplitsThem) {
  content::WebContents* contents = sidebar_contents();
  OpenThreeTabs(browser(), contents);

  // One onto the middle of Three, its right half: Three, then One.
  ASSERT_EQ(true, content::EvalJs(
                      contents, content::JsReplace(
                                    kDrag, "One",
                                    "#today .tab.row[title='Three']", 0.5)));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return model.today_root()->children().size() == 2u; }));
  const SidebarNode* split = model.today_root()->children()[1].get();
  ASSERT_EQ(split->type(), SidebarNode::Type::kSplit);
  EXPECT_EQ(split->children()[0]->title(), u"Three");
  EXPECT_EQ(split->children()[1]->title(), u"One");
  MultiContentsView* view =
      BrowserView::GetBrowserViewForBrowser(browser())->multi_contents_view();
  ASSERT_TRUE(base::test::RunUntil([&] { return view->IsInSplitView(); }));
}

// Double-clicks the tab row titled $1, types $2 into its name and presses
// Enter. Returns the row's title once it shows $3.
constexpr char kRenameTab[] = R"(
  (async (title, name, shown) => {
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
    const until = async (f) => {
      while (!f()) await new Promise(r => setTimeout(r, 20));
      return f();
    };
    const row = await until(() => [...root.querySelectorAll('.tab.row')]
        .find(r => r.title === title));
    row.dispatchEvent(new MouseEvent('dblclick', {bubbles: true}));
    const input = await until(() => root.activeElement?.classList.contains(
        'tab-title-input') && root.activeElement);
    input.value = name;
    input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter'}));
    return (await until(() => [...root.querySelectorAll('.tab.row')]
        .find(r => r.title === shown))).querySelector('.title')
        .textContent.trim();
  })($1, $2, $3))";

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, DoubleClickRenamesATab) {
  content::WebContents* contents = sidebar_contents();
  OpenThreeTabs(browser(), contents);
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  SidebarNode* two = model.today_root()->children()[1].get();
  ASSERT_EQ(two->title(), u"Two");

  EXPECT_EQ("Second", content::EvalJs(contents,
                                      content::JsReplace(kRenameTab, "Two",
                                                         "Second", "Second")));
  EXPECT_EQ(two->custom_title(), u"Second");

  // An empty name goes back to the page's title.
  EXPECT_EQ("Two", content::EvalJs(contents, content::JsReplace(
                                                 kRenameTab, "Second", "",
                                                 "Two")));
  EXPECT_TRUE(two->custom_title().empty());
}

// Presses |key| on the focused row, and returns the title of the row focused
// after it.
constexpr char kPressKey[] = R"(
  (async (key) => {
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
    root.activeElement.dispatchEvent(new KeyboardEvent('keydown', {
      key, bubbles: true, cancelable: true, composed: true,
    }));
    await new Promise(r => setTimeout(r, 0));
    return root.activeElement?.title ?? '';
  })($1))";

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, KeyboardNavigation) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_NO_FATAL_FAILURE(OpenThreeTabs(browser(), contents));
  TabStripModel* tabs = browser()->tab_strip_model();
  // Focus events need a focused page.
  contents->Focus();

  // Focusing the list lands on the active entry.
  EXPECT_EQ("Three", content::EvalJs(contents, R"(
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, FavoritesKeyboardNavigation) {
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
        document.querySelector('compass-sidebar-app').shadowRoot
            .querySelectorAll('.tile').length)")
               .ExtractInt() == 2;
  }));
  contents->Focus();

  // The list lands on the active entry; Up goes into the grid.
  EXPECT_EQ("Three", content::EvalJs(contents, R"(
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
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
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, HoverCards) {
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

IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, ArchivePanel) {
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
      const root = document.querySelector('compass-sidebar-app').shadowRoot;
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
using CompassWindowSyncBrowserTest = CompassSidebarBrowserTest;

IN_PROC_BROWSER_TEST_F(CompassWindowSyncBrowserTest,
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

IN_PROC_BROWSER_TEST_F(CompassWindowSyncBrowserTest,
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

// The load bar sits in the frame's gap above the page: sweeping while the
// server hasn't answered, then a line that fills, then gone.
IN_PROC_BROWSER_TEST_F(CompassSidebarBrowserTest, LoadBarSweepsWhileConnecting) {
  net::test_server::ControllableHttpResponse response(embedded_test_server(),
                                                      "/slow");
  ASSERT_TRUE(embedded_test_server()->Start());
  LoadBar* bar = FindDescendant<LoadBar>(browser_view());
  ASSERT_TRUE(bar);
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  // The first page's load, done and faded.
  ASSERT_TRUE(content::WaitForLoadStop(contents));
  ASSERT_TRUE(base::test::RunUntil([&] { return !bar->IsShownForTesting(); }));

  contents->GetController().LoadURL(embedded_test_server()->GetURL("/slow"),
                                    content::Referrer(),
                                    ui::PAGE_TRANSITION_TYPED, std::string());
  response.WaitForRequest();
  ASSERT_TRUE(base::test::RunUntil([&] { return bar->IsSweepingForTesting(); }));
  EXPECT_TRUE(bar->GetVisible());

  // Over the card, in the gap above it.
  views::View* card =
      BrowserView::GetBrowserViewForBrowser(browser())->multi_contents_view();
  const gfx::Rect card_bounds = views::View::ConvertRectToTarget(
      card, browser_view(), card->GetLocalBounds());
  const gfx::Rect bar_bounds = bar->bounds();
  EXPECT_GT(bar_bounds.width(), 0);
  EXPECT_LE(bar_bounds.bottom(), card_bounds.y());
  EXPECT_GE(bar_bounds.x(), card_bounds.x());
  EXPECT_LE(bar_bounds.right(), card_bounds.right());

  // The server answers: no more sweep.
  response.Send(
      "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<p>half");
  ASSERT_TRUE(base::test::RunUntil([&] {
    return bar->IsShownForTesting() && !bar->IsSweepingForTesting();
  }));

  response.Send("</p>");
  response.Done();
  ASSERT_TRUE(content::WaitForLoadStop(contents));
  ASSERT_TRUE(base::test::RunUntil([&] { return !bar->IsShownForTesting(); }));
  EXPECT_FALSE(bar->GetVisible());
}

// A space ID in the switcher's order that no profile has (its space was lost
// with a crash) is dropped once every listed profile's spaces are in.
class SpaceDirectoryStaleOrderBrowserTest : public InProcessBrowserTest {
 protected:
  static constexpr char kStaleId[] = "d530ee77-2ecf-4010-926b-5585d0d80fb6";

  bool SetUpUserDataDirectory() override {
    base::FilePath user_data_dir;
    return base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir) &&
           base::WriteFile(
               user_data_dir.AppendASCII("Compass Spaces.json"),
               base::StrCat({R"({"version": 1, "profiles": ["Default"], )",
                             R"("order": [")", kStaleId, R"("]})"}));
  }
};

IN_PROC_BROWSER_TEST_F(SpaceDirectoryStaleOrderBrowserTest,
                       DropsSpacesNoProfileHas) {
  SpaceDirectory* directory = SpaceDirectory::Get();
  ASSERT_TRUE(base::test::RunUntil([&] { return directory->IsLoaded(); }));
  EXPECT_FALSE(directory->GetSpaces().empty());
  directory->Flush();
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath user_data_dir;
  ASSERT_TRUE(base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir));
  std::string json;
  ASSERT_TRUE(base::test::RunUntil([&] {
    return base::ReadFileToString(
               user_data_dir.AppendASCII("Compass Spaces.json"), &json) &&
           json.find("\"order\"") != std::string::npos &&
           json.find(kStaleId) == std::string::npos;
  })) << json;
}

}  // namespace
}  // namespace compass
