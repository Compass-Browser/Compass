// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/now_playing_tracker.h"

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/test/run_until.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/chrome_test_path_utils.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

// The sidebar's now playing widgets: docs/design/sidebar.md, "Now playing".

namespace compass {
namespace {

using Action = NowPlayingTracker::Action;

class NowPlayingBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    // A left tab's video would go to auto picture-in-picture, which hides
    // its widget (covered on its own below). Blocking it still works.
    HostContentSettingsMapFactory::GetForProfile(browser()->GetProfile())
        ->SetDefaultContentSetting(ContentSettingsType::AUTO_PICTURE_IN_PICTURE,
                                   CONTENT_SETTING_BLOCK);
    tracker_ =
        std::make_unique<NowPlayingTracker>(browser(), base::DoNothing());
  }

  void TearDownOnMainThread() override {
    tracker_.reset();
    InProcessBrowserTest::TearDownOnMainThread();
  }

  // Opens a looping video in a new foreground tab and plays it, with sound.
  content::WebContents* OpenPlayingVideo() {
    EXPECT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser(),
        chrome_test_utils::GetTestUrl(
            base::FilePath(FILE_PATH_LITERAL("media/picture-in-picture")),
            base::FilePath(FILE_PATH_LITERAL("autopip-no-register.html"))),
        WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
    content::WebContents* contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    contents->GetPrimaryMainFrame()->ExecuteJavaScriptWithUserGestureForTests(
        u"playVideo()", base::NullCallback(),
        content::ISOLATED_WORLD_ID_GLOBAL);
    EXPECT_TRUE(base::test::RunUntil([&] {
      return content::EvalJs(contents, "!video.paused && video.currentTime > 0")
          .ExtractBool();
    }));
    return contents;
  }

  void OpenBlankTab() {
    ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser(), GURL("data:text/html,<title>Blank</title>"),
        WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  }

  void Activate(content::WebContents* contents) {
    TabStripModel* strip = browser()->tab_strip_model();
    strip->ActivateTabAt(strip->GetIndexOfWebContents(contents));
  }

  // The one widget, once there's one.
  const NowPlayingTracker::Item& WaitForItem() {
    EXPECT_TRUE(
        base::test::RunUntil([&] { return tracker_->items().size() == 1u; }));
    return tracker_->items().front();
  }

  bool WaitForNoItems() {
    return base::test::RunUntil([&] { return tracker_->items().empty(); });
  }

  // Waits out the show delay, to check that nothing showed.
  static void WaitForShowDelay() {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(),
        NowPlayingTracker::kShowDelay + base::Milliseconds(500));
    run_loop.Run();
  }

  std::unique_ptr<NowPlayingTracker> tracker_;
};

// A widget shows once the playing tab is left, and goes when it's back.
IN_PROC_BROWSER_TEST_F(NowPlayingBrowserTest, ShowsOnlyWhileTheTabIsHidden) {
  content::WebContents* video = OpenPlayingVideo();
  WaitForShowDelay();
  EXPECT_TRUE(tracker_->items().empty());

  OpenBlankTab();
  const NowPlayingTracker::Item& item = WaitForItem();
  EXPECT_EQ(item.title, u"Page that does not register for autopip");
  EXPECT_EQ(item.url, video->GetLastCommittedURL());
  EXPECT_TRUE(item.playing);
  EXPECT_FALSE(item.muted);
  EXPECT_TRUE(item.can_play_pause);
  EXPECT_TRUE(item.can_picture_in_picture);

  Activate(video);
  EXPECT_TRUE(WaitForNoItems());
}

// Pause, play and mute act on the tab, and the widget follows.
IN_PROC_BROWSER_TEST_F(NowPlayingBrowserTest, Controls) {
  content::WebContents* video = OpenPlayingVideo();
  OpenBlankTab();
  const std::string id = WaitForItem().id;

  tracker_->Act(id, Action::kPause);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !tracker_->items().empty() && !tracker_->items()[0].playing;
  }));
  EXPECT_EQ(true, content::EvalJs(video, "video.paused"));

  tracker_->Act(id, Action::kPlay);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !tracker_->items().empty() && tracker_->items()[0].playing;
  }));

  tracker_->Act(id, Action::kMute);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !tracker_->items().empty() && tracker_->items()[0].muted;
  }));
  EXPECT_TRUE(video->IsAudioMuted());
  // Muting takes the player out of the session: the widget stays as it was,
  // with only the speaker working.
  EXPECT_TRUE(tracker_->items()[0].playing);
  EXPECT_FALSE(tracker_->items()[0].can_play_pause);

  tracker_->Act(id, Action::kUnmute);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !tracker_->items().empty() && !tracker_->items()[0].muted &&
           tracker_->items()[0].can_play_pause;
  }));
  EXPECT_FALSE(video->IsAudioMuted());
}

// Closing a widget pauses its media; it comes back when it plays again.
IN_PROC_BROWSER_TEST_F(NowPlayingBrowserTest, Dismiss) {
  content::WebContents* video = OpenPlayingVideo();
  OpenBlankTab();
  tracker_->Act(WaitForItem().id, Action::kDismiss);
  ASSERT_TRUE(WaitForNoItems());
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return content::EvalJs(video, "video.paused").ExtractBool(); }));

  ASSERT_TRUE(content::ExecJs(video, "video.play()"));
  EXPECT_TRUE(WaitForItem().playing);
}

// The tab icon shows the tab.
IN_PROC_BROWSER_TEST_F(NowPlayingBrowserTest, ShowTab) {
  content::WebContents* video = OpenPlayingVideo();
  OpenBlankTab();
  tracker_->Act(WaitForItem().id, Action::kShowTab);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), video);
  EXPECT_TRUE(WaitForNoItems());
}

// Another space's tab, parked while the window shows this one, has a widget
// too; its button switches back.
IN_PROC_BROWSER_TEST_F(NowPlayingBrowserTest, AnotherSpace) {
  content::WebContents* video = OpenPlayingVideo();
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");

  features->SwitchToSpace(*work);
  ASSERT_TRUE(base::test::RunUntil([&] { return IsInParkingWindow(video); }));
  tracker_->Act(WaitForItem().id, Action::kShowTab);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return &features->space_model() == home &&
           browser()->tab_strip_model()->GetActiveWebContents() == video;
  }));
  EXPECT_TRUE(WaitForNoItems());
}

// A video in picture-in-picture has no widget: its window is the control.
IN_PROC_BROWSER_TEST_F(NowPlayingBrowserTest, PictureInPicture) {
  content::WebContents* video = OpenPlayingVideo();
  OpenBlankTab();
  tracker_->Act(WaitForItem().id, Action::kPictureInPicture);
  ASSERT_TRUE(
      base::test::RunUntil([&] { return video->HasPictureInPictureVideo(); }));
  EXPECT_TRUE(WaitForNoItems());
}

}  // namespace
}  // namespace compass
