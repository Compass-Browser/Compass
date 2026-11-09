// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/picture_in_picture/auto_picture_in_picture_tab_helper.h"
#include "chrome/browser/picture_in_picture/picture_in_picture_window_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/picture_in_picture/call_pip.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/media_switches.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "compass/browser/ui/views/picture_in_picture/pip_windows.h"
#include "ui/views/widget/widget.h"
#endif

// Picture-in-picture for calls (docs/design/picture-in-picture.md, "Calls"):
// Teams and Zoom through common/picture_in_picture/call_pip.js, and sites
// with their own document picture-in-picture window, as Meet.

namespace compass {

namespace {

// Shaped like a call: the user's own camera (the largest tile), two remote
// tiles (canvas streams, picture-in-picture switched off as Teams does), and
// Teams' buttons, whose labels say what they'd do.
constexpr char kCallPage[] = R"(<!doctype html>
<style>video { display: block; }</style>
<video id="self" width="480" height="360" muted autoplay></video>
<video id="small" width="200" height="150" muted autoplay
    disablepictureinpicture></video>
<video id="large" width="400" height="300" muted autoplay
    disablepictureinpicture></video>
<button id="microphone-button" aria-label="Mute mic"></button>
<button id="video-button" aria-label="Turn camera off"></button>
<button id="hangup-button" aria-label="Leave"></button>
<script>
  window.clicks = [];
  const flip = (id, a, b) => {
    const button = document.getElementById(id);
    button.addEventListener('click', () => {
      clicks.push(id);
      const label = button.getAttribute('aria-label');
      button.setAttribute('aria-label', label === a ? b : a);
    });
  };
  flip('microphone-button', 'Mute mic', 'Unmute mic');
  flip('video-button', 'Turn camera off', 'Turn camera on');
  document.getElementById('hangup-button')
      .addEventListener('click', () => clicks.push('hangup-button'));

  // A canvas that keeps drawing, as a remote participant.
  const remote = (color) => {
    const canvas = document.createElement('canvas');
    canvas.width = 320;
    canvas.height = 240;
    const context = canvas.getContext('2d');
    let n = 0;
    setInterval(() => {
      context.fillStyle = color;
      context.fillRect(0, 0, 320, 240);
      context.fillStyle = '#fff';
      context.fillRect(n++ % 320, 0, 10, 240);
    }, 33);
    return canvas.captureStream(30);
  };

  async function start() {
    document.getElementById('self').srcObject =
        await navigator.mediaDevices.getUserMedia({video: true, audio: true});
    document.getElementById('small').srcObject = remote('#c00');
    document.getElementById('large').srcObject = remote('#00c');
    return true;
  }
  const ready = () => [...document.querySelectorAll('video')].every(
      (v) => v.readyState >= 2 && !v.paused);
</script>)";

// A site with its own window: a document picture-in-picture one, opened
// from its enterpictureinpicture handler, as Meet does, while it uses the
// microphone.
constexpr char kOwnWindowPage[] = R"(<!doctype html>
<p>Meeting</p>
<script>
  navigator.mediaSession.setActionHandler('enterpictureinpicture', async () => {
    const pip = await documentPictureInPicture.requestWindow(
        {width: 320, height: 240});
    pip.document.body.textContent = 'Participants';
  });
  async function start() {
    window.stream =
        await navigator.mediaDevices.getUserMedia({audio: true});
    return true;
  }
</script>)";

// Zoom's web client: the call in a same-origin <iframe> of the page.
constexpr char kFramedPage[] = R"(<!doctype html>
<iframe src="/v2/call" width="1100" height="800"></iframe>)";

std::unique_ptr<net::test_server::HttpResponse> ServePage(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/html");
  const std::string& url = request.relative_url;
  response->set_content(url.starts_with("/own")      ? kOwnWindowPage
                        : url.starts_with("/framed") ? kFramedPage
                                                     : kCallPage);
  return response;
}

}  // namespace

class CompassCallPipBrowserTest : public InProcessBrowserTest {
 public:
  // Leaving the window has its own tests (CompassAutoPipBrowserTest.
  // LeavingTheWindow*). Without a desktop (a headless run, or Windows'
  // session 0) the window counts as covered, and upstream ignores tab
  // switches in a covered window.
  CompassCallPipBrowserTest() {
    features_.InitAndDisableFeature(
        media::kAutoPictureInPictureOnWindowOccluded);
  }

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(switches::kUseFakeDeviceForMediaStream);
    command_line->AppendSwitch(switches::kUseFakeUIForMediaStream);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    net::EmbeddedTestServer::ServerCertificateConfig cert;
    cert.dns_names = {"teams.microsoft.com", "app.zoom.us", "meet.example.test",
                      "example.test"};
    https_.SetSSLConfig(cert);
    https_.RegisterRequestHandler(base::BindRepeating(&ServePage));
    ASSERT_TRUE(https_.Start());
  }

  // Opens |url| in a new foreground tab and starts its call.
  content::WebContents* OpenCall(const GURL& url) {
    EXPECT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
    content::WebContents* contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    EXPECT_EQ(true, content::EvalJs(contents, "start()"));
    return contents;
  }

  // Opens a Teams-like call and waits until the script has registered the
  // call's actions.
  content::WebContents* OpenTeamsCall() {
    content::WebContents* call =
        OpenCall(https_.GetURL("teams.microsoft.com", "/v2/call"));
    EXPECT_TRUE(base::test::RunUntil(
        [&] { return content::EvalJs(call, "ready()").ExtractBool(); }));
    EXPECT_TRUE(base::test::RunUntil([&] {
      return AutoPictureInPictureTabHelper::FromWebContents(call)
          ->HasAutoPictureInPictureBeenRegistered();
    }));
    return call;
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

  // The id of the call's video in picture-in-picture, or "".
  static std::string PipVideo(content::WebContents* call) {
    return content::EvalJs(call, "document.pictureInPictureElement?.id ?? ''")
        .ExtractString();
  }

  static content::WebContents* PipContents() {
    return PictureInPictureWindowManager::GetInstance()->GetWebContents();
  }

  net::EmbeddedTestServer https_{net::EmbeddedTestServer::TYPE_HTTPS};
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, CallSites) {
  EXPECT_TRUE(IsCallPipURL(GURL("https://teams.microsoft.com/v2/")));
  EXPECT_TRUE(IsCallPipURL(GURL("https://teams.live.com/v2/")));
  EXPECT_TRUE(IsCallPipURL(GURL("https://teams.cloud.microsoft/")));
  EXPECT_TRUE(IsCallPipURL(GURL("https://app.zoom.us/wc/123/join")));
  EXPECT_TRUE(IsCallPipURL(GURL("https://us05web.zoom.us/wc/123/join")));
  EXPECT_FALSE(IsCallPipURL(GURL("https://zoom.us/pricing")));
  EXPECT_FALSE(IsCallPipURL(GURL("https://notzoom.us/wc/1")));
  EXPECT_FALSE(IsCallPipURL(GURL("https://meet.google.com/abc-defg-hij")));
  EXPECT_FALSE(IsCallPipURL(GURL("http://teams.microsoft.com/")));
}

// Leaving a call's tab shows the largest remote tile, not the user's own
// camera; coming back hides it.
IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, LeavingShowsLargestTile) {
  content::WebContents* call = OpenTeamsCall();

  OpenBlankTab();
  ASSERT_TRUE(base::test::RunUntil([&] { return PipContents() == call; }));
  EXPECT_EQ(PipVideo(call), "large");

  Activate(call);
  ASSERT_TRUE(base::test::RunUntil([&] { return !PipContents(); }));
}

// Another tile that stays the largest takes the window over.
IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, FollowsTheSpeaker) {
  content::WebContents* call = OpenTeamsCall();
  OpenBlankTab();
  ASSERT_TRUE(base::test::RunUntil([&] { return PipVideo(call) == "large"; }));

  ASSERT_TRUE(content::ExecJs(call, R"(
      small.width = 440; small.height = 330;
      large.width = 160; large.height = 120;)"));
  ASSERT_TRUE(base::test::RunUntil([&] { return PipVideo(call) == "small"; }));
  EXPECT_EQ(PipContents(), call);
}

// The window's microphone, camera and hang up buttons press the site's.
IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, ButtonsPressTheSite) {
  content::WebContents* call = OpenTeamsCall();
  OpenBlankTab();
  ASSERT_TRUE(base::test::RunUntil([&] { return PipContents() == call; }));

  content::MediaSession* session = content::MediaSession::Get(call);
  session->ToggleMicrophone();
  session->ToggleCamera();
  session->HangUp();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(call, "clicks.join()").ExtractString() ==
           "microphone-button,video-button,hangup-button";
  }));
}

// A call in a frame of the page, as Zoom's, goes too.
IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, CallInAFrame) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), https_.GetURL("teams.microsoft.com", "/framed"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* call =
      browser()->tab_strip_model()->GetActiveWebContents();
  content::RenderFrameHost* frame =
      content::ChildFrameAt(call->GetPrimaryMainFrame(), 0);
  ASSERT_TRUE(frame);
  ASSERT_EQ(true, content::EvalJs(frame, "start()"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return content::EvalJs(frame, "ready()").ExtractBool(); }));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return AutoPictureInPictureTabHelper::FromWebContents(call)
        ->HasAutoPictureInPictureBeenRegistered();
  }));

  OpenBlankTab();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(frame, "document.pictureInPictureElement?.id ?? ''")
               .ExtractString() == "large";
  }));
  EXPECT_EQ(PipContents(), call);
}

// Zoom's web client stays visible to itself when its tab is left (it drops
// its video while hidden); Teams and other pages see the real state.
IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, ZoomStaysVisible) {
  content::WebContents* zoom =
      OpenCall(https_.GetURL("app.zoom.us", "/wc/123/start"));
  EXPECT_TRUE(CallPipKeepsPageVisibleForTesting(zoom));
  content::WebContents* teams =
      OpenCall(https_.GetURL("teams.microsoft.com", "/v2/call"));
  EXPECT_FALSE(CallPipKeepsPageVisibleForTesting(teams));

  OpenBlankTab();
  EXPECT_EQ("visible", content::EvalJs(zoom, "document.visibilityState"));
  EXPECT_EQ("hidden", content::EvalJs(teams, "document.visibilityState"));

  // Off the web client, it's let go.
  ASSERT_TRUE(content::NavigateToURL(
      zoom, https_.GetURL("app.zoom.us", "/pricing")));
  EXPECT_FALSE(CallPipKeepsPageVisibleForTesting(zoom));
}

// Off a call site nothing is registered for the page.
IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, OtherSitesAreLeftAlone) {
  content::WebContents* page =
      OpenCall(https_.GetURL("example.test", "/v2/call"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return content::EvalJs(page, "ready()").ExtractBool(); }));
  // A second for the script's first update, had it been injected.
  base::RunLoop run_loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Milliseconds(1500));
  run_loop.Run();
  EXPECT_FALSE(AutoPictureInPictureTabHelper::FromWebContents(page)
                   ->HasAutoPictureInPictureBeenRegistered());
}

// A site's own document picture-in-picture window (Meet's) opens when its
// tab is left during a call, and gets Compass's window behavior: tracked for
// click-through and snapping, its page keeping its gestures.
IN_PROC_BROWSER_TEST_F(CompassCallPipBrowserTest, SitesOwnWindow) {
  content::WebContents* meeting =
      OpenCall(https_.GetURL("meet.example.test", "/own"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return AutoPictureInPictureTabHelper::FromWebContents(meeting)
        ->HasAutoPictureInPictureBeenRegistered();
  }));

  OpenBlankTab();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return PictureInPictureWindowManager::GetInstance()
               ->GetChildWebContents() != nullptr;
  }));
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return PipWindows::Get().windows_for_testing().size() == 1u; }));
  views::Widget* window = PipWindows::Get().windows_for_testing()[0];
  EXPECT_FALSE(PipWindows::Get().TakesGestures(window));
  EXPECT_FALSE(PipWindows::Get().IsClickThrough(window));
  PipWindows::Get().Toggle();
  EXPECT_TRUE(PipWindows::Get().IsIgnoringMouseForTesting(window));
  PipWindows::Get().Toggle();
  EXPECT_FALSE(PipWindows::Get().IsIgnoringMouseForTesting(window));
#endif

  Activate(meeting);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !PictureInPictureWindowManager::GetInstance()->GetChildWebContents();
  }));
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  EXPECT_TRUE(PipWindows::Get().windows_for_testing().empty());
#endif
}

}  // namespace compass
