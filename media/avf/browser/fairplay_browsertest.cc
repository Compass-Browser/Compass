// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// FairPlay (com.apple.fps) through EME (docs/design/fairplay.md): what pages
// can ask for, the CDM in the AVFoundation process, and MSE through AVPlayer.
// Offline: protected content needs a license server, so that stops at the
// session; MSE plays clear content with FairPlay's MediaKeys.

#include <string>

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/media/avf/common/avf_features.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::avf {
namespace {

// Makes MediaKeys and a session, as a page does from its "encrypted" event.
constexpr char kSession[] = R"((async () => {
  const access = await navigator.requestMediaKeySystemAccess('com.apple.fps', [{
    initDataTypes: ['skd'],
    videoCapabilities: [{contentType: 'application/vnd.apple.mpegurl'}],
  }]);
  const keys = await access.createMediaKeys();
  if ($1) await keys.setServerCertificate(new Uint8Array([1, 2, 3, 4]));
  const session = keys.createSession();
  try {
    await session.generateRequest('skd', new TextEncoder().encode($2));
    return 'session ' + session.sessionId;
  } catch (e) {
    return e.name;
  }
})())";

class FairPlayBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    base::FilePath root;
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
    embedded_test_server()->ServeFilesFromDirectory(
        root.AppendASCII("compass/media/avf/test/data"));
    ASSERT_TRUE(embedded_test_server()->Start());
    // A secure context (localhost), as EME needs.
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("/avf.html")));
  }

 protected:
  std::string Session(bool certificate, const std::string& init_data) {
    return content::EvalJs(web_contents(),
                           content::JsReplace(kSession, certificate, init_data))
        .ExtractString();
  }

  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

// Native HLS and MSE (FairPlay for MSE: H.264, HEVC and AAC in MP4, 'cenc'
// init data) work; 'sinf' and persistent licenses don't.
IN_PROC_BROWSER_TEST_F(FairPlayBrowserTest, Configurations) {
  auto access = [&](const std::string& js_config) {
    return content::EvalJs(
               web_contents(),
               "navigator.requestMediaKeySystemAccess('com.apple.fps', [" +
                   js_config + "]).then(() => 'ok', (e) => e.name)")
        .ExtractString();
  };
  EXPECT_EQ(access("{initDataTypes: ['skd'], videoCapabilities: [{contentType: "
                   "'application/vnd.apple.mpegurl'}]}"),
            "ok");
  EXPECT_EQ(access("{initDataTypes: ['cenc'], videoCapabilities: [{contentType: "
                   "'video/mp4; codecs=\"avc1.640028\"'}], audioCapabilities: "
                   "[{contentType: 'audio/mp4; codecs=\"mp4a.40.2\"'}]}"),
            "ok");
  EXPECT_EQ(access("{initDataTypes: ['cenc'], videoCapabilities: [{contentType: "
                   "'video/mp4; codecs=\"hvc1.1.6.L93.B0\"'}]}"),
            "ok");
  // AVPlayer can't play VP9 or Opus from HLS.
  EXPECT_EQ(access("{initDataTypes: ['cenc'], videoCapabilities: [{contentType: "
                   "'video/mp4; codecs=\"vp09.00.10.08\"'}]}"),
            "NotSupportedError");
  EXPECT_EQ(access("{initDataTypes: ['sinf']}"), "NotSupportedError");
  EXPECT_EQ(access("{initDataTypes: ['skd'], sessionTypes: "
                   "['persistent-license']}"),
            "NotSupportedError");
}

// FairPlay's CDM gets its players the AVFoundation renderer: an MSE stream
// (clear here, so no license server is needed) plays through AVPlayer from
// its loopback copy, to the end.
IN_PROC_BROWSER_TEST_F(FairPlayBrowserTest, PlaysMse) {
  constexpr char kPlay[] = R"((async () => {
    const type = 'video/mp4; codecs="avc1.64001f"';
    const access = await navigator.requestMediaKeySystemAccess(
        'com.apple.fps', [{initDataTypes: ['cenc'],
                           videoCapabilities: [{contentType: type}]}]);
    const video = document.querySelector('video');
    await video.setMediaKeys(await access.createMediaKeys());
    const source = new MediaSource();
    video.src = URL.createObjectURL(source);
    await new Promise((ok) => source.addEventListener('sourceopen', ok));
    const buffer = source.addSourceBuffer(type);
    for (const file of ['init.mp4', 'seg0.m4s']) {
      buffer.appendBuffer(await (await fetch(file)).arrayBuffer());
      await new Promise((ok) => buffer.addEventListener('updateend', ok,
                                                        {once: true}));
    }
    source.endOfStream();
    const ended = new Promise((ok, fail) => {
      video.addEventListener('ended', ok);
      video.addEventListener('error', () => fail(video.error.message));
    });
    await video.play();
    await ended;
    return video.videoWidth + 'x' + video.videoHeight + ' ' +
           Math.round(video.currentTime);
  })())";
  EXPECT_EQ(content::EvalJs(web_contents(), kPlay), "1280x720 6");
}

// generateRequest() needs the server certificate first and an skd:// URI;
// with both, the session waits for AVPlayer's key request.
IN_PROC_BROWSER_TEST_F(FairPlayBrowserTest, Sessions) {
  EXPECT_EQ(Session(false, "skd://key-1"), "InvalidStateError");
  EXPECT_EQ(Session(true, "not-an-skd-uri"), "TypeError");
  EXPECT_EQ(Session(true, "skd://key-1").rfind("session ", 0), 0u);
}

// Without FairPlay for MSE, com.apple.fps is native HLS only, and MSE players
// go on to Widevine.
class FairPlayMseDisabledBrowserTest : public FairPlayBrowserTest {
 public:
  FairPlayMseDisabledBrowserTest() {
    features_.InitAndDisableFeature(kFairPlayMse);
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(FairPlayMseDisabledBrowserTest, NativeHlsOnly) {
  auto access = [&](const std::string& js_config) {
    return content::EvalJs(
               web_contents(),
               "navigator.requestMediaKeySystemAccess('com.apple.fps', [" +
                   js_config + "]).then(() => 'ok', (e) => e.name)")
        .ExtractString();
  };
  EXPECT_EQ(access("{initDataTypes: ['skd'], videoCapabilities: [{contentType: "
                   "'application/vnd.apple.mpegurl'}]}"),
            "ok");
  EXPECT_EQ(access("{initDataTypes: ['skd'], videoCapabilities: [{contentType: "
                   "'video/mp4; codecs=\"avc1.640028\"'}]}"),
            "NotSupportedError");
  EXPECT_EQ(access("{initDataTypes: ['cenc'], videoCapabilities: [{contentType: "
                   "'video/mp4; codecs=\"avc1.640028\"'}]}"),
            "NotSupportedError");
}

class FairPlayDisabledBrowserTest : public FairPlayBrowserTest {
 public:
  FairPlayDisabledBrowserTest() { features_.InitAndDisableFeature(kFairPlay); }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(FairPlayDisabledBrowserTest, NotOffered) {
  EXPECT_EQ(content::EvalJs(web_contents(),
                            "navigator.requestMediaKeySystemAccess("
                            "'com.apple.fps', [{initDataTypes: ['skd']}])"
                            ".then(() => 'ok', (e) => e.name)")
                .ExtractString(),
            "NotSupportedError");
}

}  // namespace
}  // namespace compass::avf
