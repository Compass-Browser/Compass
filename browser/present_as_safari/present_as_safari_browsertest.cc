// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/present_as_safari/present_as_safari.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::present_as_safari {
namespace {

class PresentAsSafariBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    base::FilePath root;
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
    embedded_test_server()->ServeFilesFromDirectory(
        root.AppendASCII("compass/media/avf/test/data"));
    embedded_test_server()->RegisterRequestMonitor(base::BindRepeating(
        &PresentAsSafariBrowserTest::OnRequest, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
  PrefService* prefs() { return browser()->GetProfile()->GetPrefs(); }

  std::string PageUserAgent() {
    return content::EvalJs(web_contents(), "navigator.userAgent")
        .ExtractString();
  }

  // The User-Agent of the last request for `path`.
  std::string RequestUserAgent(const std::string& path) {
    base::AutoLock lock(lock_);
    for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
      if (it->relative_url == path) {
        auto header = it->headers.find("User-Agent");
        return header == it->headers.end() ? "" : header->second;
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

IN_PROC_BROWSER_TEST_F(PresentAsSafariBrowserTest, SafariForTheUsersSites) {
  const GURL page =
      embedded_test_server()->GetURL("video.example.com", "/avf.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), page));
  EXPECT_NE(PageUserAgent(), kSafariUserAgent);

  SetEnabledFor(prefs(), page, true);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), page));
  EXPECT_EQ(PageUserAgent(), kSafariUserAgent);
  EXPECT_EQ(RequestUserAgent("/avf.html"), kSafariUserAgent);
  // No client hints, as in Safari.
  EXPECT_EQ(content::EvalJs(web_contents(),
                            "!navigator.userAgentData || "
                            "navigator.userAgentData.brands.length === 0"),
            true);

  // AVPlayer's requests for the page's HLS say Safari too. (From 127.0.0.1:
  // AVPlayer resolves hosts itself, without the test's host resolver.)
  content::EvalJsResult played = content::EvalJs(
      web_contents(),
      content::JsReplace("play($1)",
                         embedded_test_server()->GetURL("/clear.m3u8")));
  ASSERT_TRUE(played.is_ok()) << played;
  EXPECT_EQ(RequestUserAgent("/seg0.m4s"), kSafariUserAgent);

  // Other sites keep Compass's user agent.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("other.test", "/avf.html")));
  EXPECT_NE(PageUserAgent(), kSafariUserAgent);
  EXPECT_NE(RequestUserAgent("/avf.html"), kSafariUserAgent);

  // Turned off, the site is back to Compass's too.
  SetEnabledFor(prefs(), page, false);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), page));
  EXPECT_NE(PageUserAgent(), kSafariUserAgent);
}

// Pages presenting as Safari get com.apple.fps as Safari offers it: players
// that use FairPlay only there (Bitmovin) probe it with "cenc" and Safari's
// legacy names. Other sites keep "skd" only, so MSE players don't pick it.
IN_PROC_BROWSER_TEST_F(PresentAsSafariBrowserTest, SafariFairPlayConfigs) {
  net::EmbeddedTestServer https(net::EmbeddedTestServer::TYPE_HTTPS);
  https.SetSSLConfig(net::EmbeddedTestServer::CERT_TEST_NAMES);
  base::FilePath root;
  ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
  https.ServeFilesFromDirectory(root.AppendASCII("compass/media/avf/test/data"));
  ASSERT_TRUE(https.Start());

  constexpr char kProbe[] = R"(
    navigator.requestMediaKeySystemAccess($1, [{
      initDataTypes: [$2],
      videoCapabilities: [{contentType: 'video/mp4; codecs="avc1.4d401e"'}],
    }]).then(() => 'ok', (e) => e.name))";
  auto probe = [&](const std::string& key_system,
                   const std::string& init_data_type) {
    return content::EvalJs(web_contents(), content::JsReplace(
                                               kProbe, key_system,
                                               init_data_type))
        .ExtractString();
  };

  const GURL safari_page = https.GetURL("video.a.test", "/avf.html");
  SetEnabledFor(prefs(), safari_page, true);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), safari_page));
  EXPECT_EQ(probe("com.apple.fps", "skd"), "ok");
  EXPECT_EQ(probe("com.apple.fps.1_0", "cenc"), "ok");
  EXPECT_EQ(probe("com.apple.fps.3_0", "cenc"), "ok");
  EXPECT_EQ(probe("com.apple.fairplay", "cenc"), "NotSupportedError");

  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), https.GetURL("b.test", "/avf.html")));
  EXPECT_EQ(probe("com.apple.fps", "skd"), "ok");
  EXPECT_EQ(probe("com.apple.fps.1_0", "skd"), "ok");
  EXPECT_EQ(probe("com.apple.fps.1_0", "cenc"), "NotSupportedError");
}

}  // namespace
}  // namespace compass::present_as_safari
