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

#if BUILDFLAG(IS_MAC)
#include "compass/browser/ui/views/picture_in_picture/pip_click_through_mac.h"
#endif

// Auto picture-in-picture as Compass sets it up: see
// docs/design/picture-in-picture.md.

namespace compass {
namespace {

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

#if BUILDFLAG(IS_MAC)
// Ctrl+Option+Cmd+P lets clicks through the picture-in-picture window and
// back; the shortcut listens only while a window is open, and click-through
// ends with the window.
IN_PROC_BROWSER_TEST_F(CompassAutoPipBrowserTest, ClickThroughShortcut) {
  PipClickThrough& click_through = PipClickThrough::Get();
  EXPECT_FALSE(click_through.is_listening_for_testing());

  content::WebContents* video = OpenPlayingVideo(browser());
  OpenBlankTab(browser());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return IsInAutoPip(video) &&
           !click_through.windows_for_testing().empty();
  }));
  views::Widget* window = click_through.windows_for_testing()[0];
  EXPECT_TRUE(click_through.is_listening_for_testing());
  EXPECT_FALSE(click_through.IsIgnoringMouseForTesting(window));
  EXPECT_EQ(click_through.GetCornerRadiusForTesting(window),
            PipClickThrough::kCornerRadius);

  click_through.OnKeyPressed(PipClickThrough::GetAccelerator());
  EXPECT_TRUE(click_through.enabled());
  EXPECT_TRUE(click_through.IsIgnoringMouseForTesting(window));
  EXPECT_TRUE(click_through.IsClickThrough(window));

  click_through.OnKeyPressed(PipClickThrough::GetAccelerator());
  EXPECT_FALSE(click_through.enabled());
  EXPECT_FALSE(click_through.IsIgnoringMouseForTesting(window));
  EXPECT_FALSE(click_through.IsHoveredForTesting(window));

  // On again, then the window goes: off, and no longer listening.
  click_through.OnKeyPressed(PipClickThrough::GetAccelerator());
  Activate(browser(), video);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return click_through.windows_for_testing().empty(); }));
  EXPECT_FALSE(click_through.enabled());
  EXPECT_FALSE(click_through.is_listening_for_testing());
}
#endif  // BUILDFLAG(IS_MAC)

}  // namespace
}  // namespace compass
