// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Native HLS through AVFoundation (docs/design/fairplay.md): a <video> with
// an .m3u8 plays in the AVFoundation utility process, with the page's cookies.

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/thread_annotations.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/media/avf/common/avf_features.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::avf {
namespace {

class AVFHlsBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    base::FilePath root;
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
    embedded_test_server()->ServeFilesFromDirectory(
        root.AppendASCII("compass/media/avf/test/data"));
    embedded_test_server()->RegisterRequestMonitor(base::BindRepeating(
        &AVFHlsBrowserTest::OnRequest, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  // The Cookie headers of the requests for `path`.
  std::vector<std::string> CookiesFor(const std::string& path) {
    base::AutoLock lock(lock_);
    std::vector<std::string> cookies;
    for (const auto& request : requests_) {
      if (request.relative_url == path) {
        auto it = request.headers.find("Cookie");
        cookies.push_back(it == request.headers.end() ? "" : it->second);
      }
    }
    return cookies;
  }

  // The `header` of the first request for `path`, or "" ("none" if there
  // was no such request).
  std::string HeaderFor(const std::string& path, const std::string& header) {
    base::AutoLock lock(lock_);
    for (const auto& request : requests_) {
      if (request.relative_url == path) {
        auto it = request.headers.find(header);
        return it == request.headers.end() ? "" : it->second;
      }
    }
    return "none";
  }

 private:
  void OnRequest(const net::test_server::HttpRequest& request) {
    base::AutoLock lock(lock_);
    requests_.push_back(request);
  }

  base::Lock lock_;
  std::vector<net::test_server::HttpRequest> requests_ GUARDED_BY(lock_);
};

IN_PROC_BROWSER_TEST_F(AVFHlsBrowserTest, PlaysHls) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/avf.html")));
  content::EvalJsResult result =
      content::EvalJs(web_contents(), "play('clear.m3u8')");
  ASSERT_TRUE(result.is_ok()) << result;
  std::string json = result.ExtractString();
  // The duration and size come from AVPlayer.
  EXPECT_NE(json.find("\"duration\":6"), std::string::npos) << json;
  EXPECT_NE(json.find("\"width\":1280"), std::string::npos) << json;
  EXPECT_NE(json.find("\"height\":720"), std::string::npos) << json;

  // AVPlayer, not Chromium's HLS demuxer, fetched the segment (AVFoundation
  // marks its HLS requests with X-Playback-Session-Id), with Chromium's user
  // agent.
  EXPECT_NE(HeaderFor("/seg0.m4s", "X-Playback-Session-Id"), "");
  EXPECT_NE(HeaderFor("/seg0.m4s", "X-Playback-Session-Id"), "none");
  EXPECT_NE(HeaderFor("/seg0.m4s", "User-Agent").find("Chrome/"),
            std::string::npos);
}

IN_PROC_BROWSER_TEST_F(AVFHlsBrowserTest, SendsFirstPartyCookies) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/avf.html")));
  ASSERT_TRUE(content::ExecJs(web_contents(),
                              "document.cookie = 'session=avf-test; path=/'"));
  content::EvalJsResult result =
      content::EvalJs(web_contents(), "play('clear.m3u8')");
  ASSERT_TRUE(result.is_ok()) << result;
  for (const std::string& path : {"/clear.m3u8", "/init.mp4", "/seg0.m4s"}) {
    std::vector<std::string> cookies = CookiesFor(path);
    ASSERT_FALSE(cookies.empty()) << path;
    EXPECT_NE(cookies.back().find("session=avf-test"), std::string::npos)
        << path << ": " << cookies.back();
  }
}

class AVFHlsDisabledBrowserTest : public AVFHlsBrowserTest {
 public:
  AVFHlsDisabledBrowserTest() {
    features_.InitAndDisableFeature(kAVFoundationHls);
  }

 private:
  base::test::ScopedFeatureList features_;
};

// With the feature off, Chromium's HLS demuxer fetches the playlist.
IN_PROC_BROWSER_TEST_F(AVFHlsDisabledBrowserTest, UsesChromiumHls) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/avf.html")));
  ASSERT_TRUE(content::ExecJs(
      web_contents(), "document.getElementById('v').src = 'clear.m3u8'"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !CookiesFor("/clear.m3u8").empty(); }));
  EXPECT_EQ(HeaderFor("/clear.m3u8", "X-Playback-Session-Id"), "");
}

}  // namespace
}  // namespace compass::avf
