// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_path.h"
#include "build/build_config.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/run_until.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/picture_in_picture/auto_picture_in_picture_tab_helper.h"
#include "chrome/browser/picture_in_picture/picture_in_picture_window_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/recently_audible_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/base/base_window.h"
#include "chrome/test/base/chrome_test_path_utils.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "chrome/browser/ui/views/overlay/back_to_tab_button.h"
#include "chrome/browser/ui/views/overlay/close_image_button.h"
#include "chrome/browser/ui/views/overlay/minimize_button.h"
#include "chrome/browser/ui/views/overlay/playback_image_button.h"
#include "chrome/browser/ui/views/overlay/video_overlay_window_views.h"
#include "compass/browser/ui/views/picture_in_picture/pip_style.h"
#include "compass/browser/ui/views/picture_in_picture/pip_windows.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/rect.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/win/windows_version.h"
#include "ui/views/win/hwnd_util.h"
#endif

// Auto picture-in-picture as Compass sets it up: see
// docs/design/picture-in-picture.md.

namespace compass {
namespace {

#if BUILDFLAG(IS_WIN)
// Hot keys need an interactive window station: a test run over ssh (session
// 0) can't register any, so there it checks the rest.
bool CanRegisterHotKeys() {
  USEROBJECTFLAGS flags = {};
  return ::GetUserObjectInformation(::GetProcessWindowStation(), UOI_FLAGS,
                                    &flags, sizeof(flags), nullptr) &&
         (flags.dwFlags & WSF_VISIBLE);
}
#elif BUILDFLAG(IS_MAC)
bool CanRegisterHotKeys() {
  return true;
}
#endif

class CompassAutoPipBrowserTest : public InProcessBrowserTest {
 protected:
  // A page with a looping video that doesn't ask for auto picture-in-picture.
  // A file URL: upstream allows https and file.
  static GURL VideoPage() {
    return chrome_test_utils::GetTestUrl(
        base::FilePath(FILE_PATH_LITERAL("media/picture-in-picture")),
        base::FilePath(FILE_PATH_LITERAL("autopip-no-register.html")));
  }

  // Opens the video page in a new foreground tab of |browser| and plays it,
  // with sound unless |muted|.
  content::WebContents* OpenPlayingVideo(BrowserWindowInterface* browser,
                                         bool muted = false) {
    EXPECT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser, VideoPage(), WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
    content::WebContents* contents =
        browser->GetTabStripModel()->GetActiveWebContents();
    if (muted) {
      EXPECT_TRUE(content::ExecJs(contents, "muteVideo()"));
    }
    contents->GetPrimaryMainFrame()->ExecuteJavaScriptWithUserGestureForTests(
        u"playVideo()", base::NullCallback(),
        content::ISOLATED_WORLD_ID_GLOBAL);
    EXPECT_TRUE(base::test::RunUntil([&] {
      return content::EvalJs(contents, "!video.paused && video.currentTime > 0")
          .ExtractBool();
    }));
    if (!muted) {
      EXPECT_TRUE(base::test::RunUntil([&] {
        return RecentlyAudibleHelper::FromWebContents(contents)
            ->WasRecentlyAudible();
      }));
    }
    return contents;
  }

  void OpenBlankTab(BrowserWindowInterface* browser) {
    ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser, GURL("data:text/html,<title>Blank</title>"),
        WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  }

  static void Activate(BrowserWindowInterface* browser,
                       content::WebContents* contents) {
    TabStripModel* strip = browser->GetTabStripModel();
    strip->ActivateTabAt(strip->GetIndexOfWebContents(contents));
  }

  // The tab whose video shows in picture-in-picture, or null.
  static content::WebContents* PipContents() {
    return PictureInPictureWindowManager::GetInstance()->GetWebContents();
  }

  static bool IsInAutoPip(content::WebContents* contents) {
    return PipContents() == contents && contents->HasPictureInPictureVideo() &&
           AutoPictureInPictureTabHelper::FromWebContents(contents)
               ->IsInAutoPictureInPicture();
  }
};

// Leaving a tab with a playing video shows it in picture-in-picture, with no
// prompt and no media engagement; coming back hides it.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, LeavingAndComingBack) {
  content::WebContents* video = OpenPlayingVideo(browser());

  OpenBlankTab(browser());
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(video); }));

  Activate(browser(), video);
  ASSERT_TRUE(base::test::RunUntil([&] { return !PipContents(); }));
  EXPECT_FALSE(video->HasPictureInPictureVideo());
}

// Of two tabs playing, the one left last shows.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, TheTabLeftLastShows) {
  content::WebContents* first = OpenPlayingVideo(browser());
  content::WebContents* second = OpenPlayingVideo(browser());
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(first); }));

  OpenBlankTab(browser());
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(second); }));
  EXPECT_FALSE(first->HasPictureInPictureVideo());

  // And back the other way.
  Activate(browser(), first);
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(second); }));
  Activate(browser(), second);
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(first); }));
}

// Muted videos stay put.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, MutedVideoDoesNotShow) {
  OpenPlayingVideo(browser(), /*muted=*/true);
  OpenBlankTab(browser());
  // Long enough for the URL check (at most 500 ms) and the request.
  base::RunLoop run_loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Seconds(1));
  run_loop.Run();
  EXPECT_FALSE(PipContents());
}

// Switching spaces leaves the shown tab too.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, SwitchingSpaces) {
  content::WebContents* video = OpenPlayingVideo(browser());
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");

  features->SwitchToSpace(*work);
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(video); }));

  features->SwitchToSpace(*home);
  ASSERT_TRUE(base::test::RunUntil([&] { return !PipContents(); }));
  EXPECT_EQ(browser()->GetTabStripModel()->GetActiveWebContents(), video);
}

// So does swapping the window to another profile's space, which parks the
// tab (docs/design/spaces.md, slice 3).
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, SwappingProfiles) {
  content::WebContents* video = OpenPlayingVideo(browser());
  Profile* profile = browser()->GetProfile();
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  SpaceDirectory::Get()->List(&other);
  SpaceModel& other_home =
      SpaceService::Get(&other)->space_list().active_space();

  BrowserWindowInterface* swapped =
      SwitchWindowToSpace(browser(), &other, other_home);
  ASSERT_TRUE(swapped);
  SetBrowser(swapped);
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInParkingWindow(video); }));
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(video); }));

  // Back: the tab comes out of the parking window and is shown again.
  BrowserWindowInterface* back = SwitchWindowToSpace(
      swapped, profile, SpaceService::Get(profile)->space_list().active_space());
  ASSERT_TRUE(back);
  SetBrowser(back);
  ASSERT_TRUE(base::test::RunUntil([&] { return !PipContents(); }));
  EXPECT_EQ(back->GetTabStripModel()->GetActiveWebContents(), video);
}

// The window's "back to tab" button, as in Arc: the tab's window switches
// back to its space, and the hidden parking window never shows.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, BackToTabSwitchesSpaces) {
  content::WebContents* video = OpenPlayingVideo(browser());
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");
  features->SwitchToSpace(*work);
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(video); }));
  ASSERT_TRUE(IsInParkingWindow(video));

  ASSERT_TRUE(PictureInPictureWindowManager::GetInstance()
                  ->ExitPictureInPictureViaWindowUi(
                      PictureInPictureWindowManager::UiBehavior::
                          kCloseWindowAndFocusOpener));
  ASSERT_TRUE(base::test::RunUntil([&] { return !PipContents(); }));
  EXPECT_EQ(&features->space_model(), home);
  EXPECT_EQ(browser()->GetTabStripModel()->GetActiveWebContents(), video);
  BrowserWindowInterface* parking = features->service().parking_window();
  EXPECT_TRUE(!parking || !parking->GetWindow()->IsVisible());
}

// Also when the tab is another profile's: the window switches profiles.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, BackToTabSwitchesProfiles) {
  content::WebContents* video = OpenPlayingVideo(browser());
  Profile* profile = browser()->GetProfile();
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  SpaceDirectory::Get()->List(&other);
  SpaceModel& other_home =
      SpaceService::Get(&other)->space_list().active_space();
  BrowserWindowInterface* swapped =
      SwitchWindowToSpace(browser(), &other, other_home);
  ASSERT_TRUE(swapped);
  SetBrowser(swapped);
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInAutoPip(video); }));

  ASSERT_TRUE(PictureInPictureWindowManager::GetInstance()
                  ->ExitPictureInPictureViaWindowUi(
                      PictureInPictureWindowManager::UiBehavior::
                          kCloseWindowAndFocusOpener));
  BrowserWindowInterface* back = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(video);
    back = tab ? tab->GetBrowserWindowInterface() : nullptr;
    return back && !IsInParkingWindow(video) &&
           back->GetTabStripModel()->GetActiveWebContents() == video;
  }));
  SetBrowser(back);
  EXPECT_EQ(back->GetProfile(), profile);
  ASSERT_TRUE(base::test::RunUntil([&] { return !PipContents(); }));
  BrowserWindowInterface* parking =
      SpaceService::Get(profile)->parking_window();
  EXPECT_TRUE(!parking || !parking->GetWindow()->IsVisible());
}

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
// Ctrl+Option+Cmd+P (Shift+Alt+P on Windows) lets clicks through the
// picture-in-picture window and back; the shortcut listens only while a
// window is open, and click-through ends with the window.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, ClickThroughShortcut) {
  PipWindows& click_through = PipWindows::Get();
  EXPECT_FALSE(click_through.is_listening_for_testing());

  content::WebContents* video = OpenPlayingVideo(browser());
  OpenBlankTab(browser());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return IsInAutoPip(video) &&
           !click_through.windows_for_testing().empty();
  }));
  views::Widget* window = click_through.windows_for_testing()[0];
  EXPECT_EQ(click_through.is_listening_for_testing(), CanRegisterHotKeys());
  EXPECT_FALSE(click_through.IsIgnoringMouseForTesting(window));
#if BUILDFLAG(IS_MAC)
  EXPECT_TRUE(click_through.HasRoundCornersForTesting(window));
  EXPECT_FALSE(click_through.AllowsTilingForTesting(window));
#else
  // The system's rounded corners, which Windows 10 doesn't have.
  EXPECT_EQ(click_through.HasRoundCornersForTesting(window),
            base::win::GetVersion() >= base::win::Version::WIN11);
#endif

  click_through.OnKeyPressed(PipWindows::GetAccelerator());
  EXPECT_TRUE(click_through.enabled());
  EXPECT_TRUE(click_through.IsIgnoringMouseForTesting(window));
  EXPECT_TRUE(click_through.IsClickThrough(window));

  click_through.OnKeyPressed(PipWindows::GetAccelerator());
  EXPECT_FALSE(click_through.enabled());
  EXPECT_FALSE(click_through.IsIgnoringMouseForTesting(window));
  EXPECT_FALSE(click_through.IsHoveredForTesting(window));

  // On again, then the window goes: off, and no longer listening.
  click_through.OnKeyPressed(PipWindows::GetAccelerator());
  Activate(browser(), video);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return click_through.windows_for_testing().empty(); }));
  EXPECT_FALSE(click_through.enabled());
  EXPECT_FALSE(click_through.is_listening_for_testing());
}

// Compass's controls (PipStyle): no site name, back to tab top left,
// minimize and close top right, each group on its pill inside the window,
// and the window's colors seeded with the tab's space color.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, PipStyle) {
  WindowFeatures::From(browser())->space_model().SetColor(SpaceColor::kBlue);
  content::WebContents* video = OpenPlayingVideo(browser());
  OpenBlankTab(browser());
  PipWindows& pip_windows = PipWindows::Get();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return IsInAutoPip(video) && !pip_windows.windows_for_testing().empty();
  }));
  auto* window =
      static_cast<VideoOverlayWindowViews*>(pip_windows.windows_for_testing()[0]
                                                .get());
  window->ForceControlsVisibleForTesting(true);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !window->IsLayoutPendingForTesting(); }));

  EXPECT_EQ(window->user_color_override(), GetSpaceTint(SpaceColor::kBlue));
  EXPECT_FALSE(window->favicon_view_for_testing()->IsDrawn());
  EXPECT_FALSE(window->origin_for_testing()->IsDrawn());

  const gfx::Size size = window->GetBounds().size();
  const gfx::Rect back = window->back_to_tab_button_for_testing()->bounds();
  const gfx::Rect minimize = window->minimize_button_for_testing()->bounds();
  const gfx::Rect close = window->close_button_for_testing()->bounds();
  EXPECT_LT(back.right(), size.width() / 2);
  EXPECT_GT(minimize.x(), size.width() / 2);
  EXPECT_EQ(minimize.right(), close.x());
  EXPECT_EQ(back.y(), close.y());
  EXPECT_EQ(close.width(), 32);
  // Back to tab carries its title.
  EXPECT_GT(back.width(), 32 + 40);

  const PipStyle::PillBounds pills = PipStyle::GetPillBoundsForTesting(window);
  EXPECT_TRUE(pills.top_leading.Contains(back));
  EXPECT_TRUE(pills.top_trailing.Contains(minimize));
  EXPECT_TRUE(pills.top_trailing.Contains(close));
  EXPECT_EQ(pills.top_leading.origin(),
            gfx::Point(PipStyle::kMargin, PipStyle::kMargin));
  EXPECT_EQ(pills.top_trailing.right(), size.width() - PipStyle::kMargin);
  EXPECT_TRUE(pills.center.Contains(
      window->play_pause_controls_view_for_testing()->bounds()));
  ASSERT_FALSE(pills.bottom.IsEmpty());
  EXPECT_LE(pills.bottom.bottom(), size.height() - PipStyle::kMargin);
  EXPECT_GE(pills.bottom.x(), PipStyle::kMargin);
  EXPECT_LE(pills.bottom.right(), size.width() - PipStyle::kMargin);
  // One bottom row: the time left of the timeline, on its line.
  const gfx::Rect time = window->timestamp_for_testing()->bounds();
  const gfx::Rect timeline = window->progress_view_for_testing()->bounds();
  EXPECT_LT(time.right(), timeline.x());
  EXPECT_EQ(time.CenterPoint().y(), timeline.CenterPoint().y());
  EXPECT_LE(pills.bottom.height(), 40);

  // A color change follows at the next layout (a resize here).
  WindowFeatures::From(browser())->space_model().SetColor(SpaceColor::kGreen);
  pip_windows.ZoomWindow(window, 0.1);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return window->user_color_override() == GetSpaceTint(SpaceColor::kGreen);
  }));
}

// Pinch keeps the aspect ratio and the center, within the sizes.
TEST(PipWindowsGeometryTest, Zoom) {
  const gfx::Rect bounds(100, 100, 400, 200);
  EXPECT_EQ(PipWindows::Zoom(bounds, 0.5, gfx::Size(), gfx::Size()),
            gfx::Rect(0, 50, 600, 300));
  EXPECT_EQ(PipWindows::Zoom(bounds, -0.5, gfx::Size(300, 100), gfx::Size()),
            gfx::Rect(150, 125, 300, 150));
  EXPECT_EQ(PipWindows::Zoom(bounds, 1, gfx::Size(), gfx::Size(1000, 300)),
            gfx::Rect(0, 50, 600, 300));
}

// Moves stay on screen; ends near an edge snap to it.
TEST(PipWindowsGeometryTest, MoveAndSnap) {
  const gfx::Rect work_area(0, 25, 1000, 800);
  EXPECT_EQ(PipWindows::Move(gfx::Rect(100, 100, 200, 100),
                             gfx::Vector2d(-500, 20), work_area),
            gfx::Rect(0, 120, 200, 100));
  // Close to the left and bottom edges.
  EXPECT_EQ(PipWindows::Snap(gfx::Rect(20, 700, 200, 100), work_area),
            gfx::Rect(PipWindows::kEdgeMargin,
                      825 - PipWindows::kEdgeMargin - 100, 200, 100));
  // Close to the right and top.
  EXPECT_EQ(PipWindows::Snap(gfx::Rect(790, 40, 200, 100), work_area),
            gfx::Rect(1000 - PipWindows::kEdgeMargin - 200,
                      25 + PipWindows::kEdgeMargin, 200, 100));
  // Away from every edge: stays.
  EXPECT_EQ(PipWindows::Snap(gfx::Rect(400, 300, 200, 100), work_area),
            gfx::Rect(400, 300, 200, 100));
}

// Pinch, two-finger moves and the snap act on the real window.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, PipGestures) {
  PipWindows& pip_windows = PipWindows::Get();
  content::WebContents* video = OpenPlayingVideo(browser());
  OpenBlankTab(browser());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return IsInAutoPip(video) && !pip_windows.windows_for_testing().empty();
  }));
  views::Widget* window = pip_windows.windows_for_testing()[0];
  const gfx::Rect before = window->GetWindowBoundsInScreen();

  pip_windows.ZoomWindow(window, 0.2);
  const gfx::Rect zoomed = window->GetWindowBoundsInScreen();
  EXPECT_GT(zoomed.width(), before.width());
  EXPECT_NEAR(static_cast<double>(zoomed.width()) / zoomed.height(),
              static_cast<double>(before.width()) / before.height(), 0.02);

  // Far to the left: stops at the edge, then snaps to the margin.
  pip_windows.MoveWindow(window, gfx::Vector2d(-100000, 0));
  const gfx::Rect work_area = display::Screen::Get()
                                  ->GetDisplayMatching(
                                      window->GetWindowBoundsInScreen())
                                  .work_area();
  EXPECT_EQ(window->GetWindowBoundsInScreen().x(), work_area.x());
  pip_windows.EndGesture(window, /*animate=*/false);
  EXPECT_EQ(window->GetWindowBoundsInScreen().x(),
            work_area.x() + PipWindows::kEdgeMargin);
}

#if BUILDFLAG(IS_WIN)
// Windows sends a touchpad's pinch as Ctrl+wheel and its two-finger scrolls
// as wheel steps finer than a notch: they resize and move the window. A
// mouse wheel's notches don't.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, PipWheelGestures) {
  PipWindows& pip_windows = PipWindows::Get();
  content::WebContents* video = OpenPlayingVideo(browser());
  OpenBlankTab(browser());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return IsInAutoPip(video) && !pip_windows.windows_for_testing().empty();
  }));
  views::Widget* window = pip_windows.windows_for_testing()[0];
  HWND hwnd = views::HWNDForWidget(window);
  ASSERT_TRUE(hwnd);
  // Away from the edges, so nothing is kept on screen or snapped.
  const gfx::Rect work_area = display::Screen::Get()
                                  ->GetDisplayMatching(
                                      window->GetWindowBoundsInScreen())
                                  .work_area();
  // (Widget::CenterWindow() left it at the right edge of an 800x600 screen.)
  const auto center = [&](const gfx::Size& size) {
    gfx::Rect bounds(size);
    bounds.set_origin(work_area.CenterPoint() -
                      gfx::Vector2d(size.width() / 2, size.height() / 2));
    window->SetBounds(bounds);
  };
  center(window->GetWindowBoundsInScreen().size());
  const gfx::Rect before = window->GetWindowBoundsInScreen();
  ASSERT_TRUE(work_area.Contains(before));

  // A notch of a mouse wheel: nothing.
  ::SendMessage(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
  EXPECT_EQ(window->GetWindowBoundsInScreen(), before);

  // Ctrl+wheel: a pinch out, keeping the aspect ratio.
  ::SendMessage(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 2 * WHEEL_DELTA),
                0);
  const gfx::Rect grown = window->GetWindowBoundsInScreen();
  SCOPED_TRACE(testing::Message() << "work area " << work_area.ToString()
                                  << ", before " << before.ToString()
                                  << ", zoomed " << grown.ToString());
  EXPECT_GT(grown.width(), before.width());
  EXPECT_NEAR(static_cast<double>(grown.width()) / grown.height(),
              static_cast<double>(before.width()) / before.height(), 0.02);

  // Back to the middle, with room to move either way.
  center(grown.size());
  const gfx::Rect zoomed = window->GetWindowBoundsInScreen();
  ASSERT_GE(zoomed.x() - work_area.x(), 50);
  ASSERT_GE(work_area.right() - zoomed.right(), 50);

  // A finer horizontal step: a move sideways (which way depends on the
  // touchpad's scrolling direction).
  ::SendMessage(hwnd, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2), 0);
  const gfx::Rect moved = window->GetWindowBoundsInScreen();
  EXPECT_NE(moved.x(), zoomed.x());
  EXPECT_EQ(moved.y(), zoomed.y());
  EXPECT_EQ(moved.size(), zoomed.size());

  // While it lets clicks through, it gets no such messages; these are sent
  // to it directly, so they would act: make sure they're passed on.
  pip_windows.Toggle();
  ::SendMessage(hwnd, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2), 0);
  EXPECT_EQ(window->GetWindowBoundsInScreen(), moved);
  pip_windows.Toggle();
}
#endif  // BUILDFLAG(IS_WIN)
#endif  // BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)

}  // namespace
}  // namespace compass
