// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/adblock/adblock_service.h"
#include "compass/browser/adblock/filter_lists.h"
#include "compass/browser/adblock/page_activation_throttle.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::adblock {
namespace {

// What the test list blocks and hides.
constexpr char kTestList[] =
    "[Adblock Plus 2.0]\n"
    "! Title: Test list\n"
    "/adblock/ad.js\n"
    "/adblock/ad.json$xhr\n"
    "##.ad-box\n"
    "a.test##.only-a\n"
    "c.test#@#.ad-box\n";

constexpr char kPage[] = R"(<!doctype html>
<style>.strong { display: block !important; }</style>
<div class="ad-box" id="generic">ad</div>
<div class="ad-box strong" id="strong">ad</div>
<div class="only-a" id="site">ad</div>
<div class="content" id="content">content</div>
<script>window.adRan = false; window.okRan = false;</script>
<script src="/adblock/ad.js"></script>
<script src="/adblock/ok.js"></script>
)";

class AdBlockBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &AdBlockBrowserTest::Serve, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());

    // Only the test list, as EasyList's text: no download reaches out.
    AdBlockService* service = AdBlockService::Get();
    ASSERT_TRUE(service);
    service->SetListEnabled(kEasyPrivacyId, false);
    service->SetListEnabled(kCookiesId, false);
    base::RunLoop published;
    service->SetListTextForTesting(kEasyListId, kTestList,
                                   published.QuitClosure());
    published.Run();
  }

  std::unique_ptr<net::test_server::HttpResponse> Serve(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    const std::string path(request.GetURL().path());
    if (path == "/adblock/page.html" || path == "/adblock/frame.html") {
      response->set_content_type("text/html");
      response->set_content(kPage);
    } else if (path == "/adblock/ad.js") {
      response->set_content_type("text/javascript");
      response->set_content("window.adRan = true;");
    } else if (path == "/adblock/ok.js") {
      response->set_content_type("text/javascript");
      response->set_content("window.okRan = true;");
    } else if (path == "/adblock/ad.json") {
      response->set_content_type("application/json");
      response->set_content("{}");
    } else {
      return nullptr;
    }
    return response;
  }

  GURL PageUrl(const std::string& host) {
    return embedded_test_server()->GetURL(host, "/adblock/page.html");
  }

  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  void Load(const std::string& host) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), PageUrl(host)));
  }

  static bool Eval(const content::ToRenderFrameHost& frame,
                   const std::string& script) {
    return content::EvalJs(frame, script).ExtractBool();
  }

  static std::string Display(const content::ToRenderFrameHost& frame,
                             const std::string& id) {
    return content::EvalJs(
               frame, "getComputedStyle(document.getElementById('" + id +
                          "')).display")
        .ExtractString();
  }

  static bool FetchBlocked(const content::ToRenderFrameHost& frame) {
    return content::EvalJs(frame,
                           "fetch('/adblock/ad.json').then(() => false, "
                           "() => true)")
        .ExtractBool();
  }
};

IN_PROC_BROWSER_TEST_F(AdBlockBrowserTest, BlocksAndHides) {
  Load("a.test");
  EXPECT_FALSE(Eval(web_contents(), "window.adRan"));
  EXPECT_TRUE(Eval(web_contents(), "window.okRan"));
  EXPECT_TRUE(FetchBlocked(web_contents()));
  EXPECT_EQ(Display(web_contents(), "generic"), "none");
  // A user !important rule wins over the page's own !important.
  EXPECT_EQ(Display(web_contents(), "strong"), "none");
  EXPECT_EQ(Display(web_contents(), "site"), "none");
  EXPECT_EQ(Display(web_contents(), "content"), "block");
}

IN_PROC_BROWSER_TEST_F(AdBlockBrowserTest, SiteRulesAndExceptions) {
  Load("b.test");
  EXPECT_EQ(Display(web_contents(), "generic"), "none");
  EXPECT_EQ(Display(web_contents(), "site"), "block");

  Load("c.test");
  EXPECT_EQ(Display(web_contents(), "generic"), "block");
  EXPECT_FALSE(Eval(web_contents(), "window.adRan"));
}

IN_PROC_BROWSER_TEST_F(AdBlockBrowserTest, OffForASite) {
  HostContentSettingsMap* settings =
      HostContentSettingsMapFactory::GetForProfile(browser()->GetProfile());
  SetAdsAllowedOn(settings, PageUrl("a.test"), true);
  EXPECT_FALSE(IsAdBlockActiveFor(browser()->GetProfile(), PageUrl("a.test")));

  Load("a.test");
  EXPECT_TRUE(Eval(web_contents(), "window.adRan"));
  EXPECT_FALSE(FetchBlocked(web_contents()));
  EXPECT_EQ(Display(web_contents(), "generic"), "block");

  // Other sites still block.
  Load("b.test");
  EXPECT_FALSE(Eval(web_contents(), "window.adRan"));

  SetAdsAllowedOn(settings, PageUrl("a.test"), false);
  Load("a.test");
  EXPECT_FALSE(Eval(web_contents(), "window.adRan"));
}

IN_PROC_BROWSER_TEST_F(AdBlockBrowserTest, OffEverywhere) {
  g_browser_process->local_state()->SetBoolean(prefs::kEnabled, false);
  Load("a.test");
  EXPECT_TRUE(Eval(web_contents(), "window.adRan"));
  EXPECT_EQ(Display(web_contents(), "generic"), "block");

  g_browser_process->local_state()->SetBoolean(prefs::kEnabled, true);
  Load("a.test");
  EXPECT_FALSE(Eval(web_contents(), "window.adRan"));
}

IN_PROC_BROWSER_TEST_F(AdBlockBrowserTest, CrossSiteFrame) {
  Load("b.test");
  const GURL frame_url =
      embedded_test_server()->GetURL("a.test", "/adblock/frame.html");
  ASSERT_TRUE(content::ExecJs(
      web_contents(),
      "new Promise(resolve => { const f = document.createElement('iframe');"
      "f.onload = resolve; f.src = '" +
          frame_url.spec() + "'; document.body.appendChild(f); })"));
  content::RenderFrameHost* frame =
      content::ChildFrameAt(web_contents()->GetPrimaryMainFrame(), 0);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(Eval(frame, "window.adRan"));
  EXPECT_TRUE(Eval(frame, "window.okRan"));
  EXPECT_EQ(Display(frame, "generic"), "none");
  // Site rules go by the frame's own site.
  EXPECT_EQ(Display(frame, "site"), "none");
}

IN_PROC_BROWSER_TEST_F(AdBlockBrowserTest, NoAdsBlockedUi) {
  Load("a.test");
  EXPECT_FALSE(Eval(web_contents(), "window.adRan"));
  // No "Ads blocked" site data written for the site (upstream's UI throttle
  // keeps it), so its site settings only change when the user changes them.
  HostContentSettingsMap* settings =
      HostContentSettingsMapFactory::GetForProfile(browser()->GetProfile());
  EXPECT_FALSE(settings->GetWebsiteSetting(PageUrl("a.test"), GURL(),
                                           ContentSettingsType::ADS_DATA,
                                           nullptr)
                   .is_dict());
}

}  // namespace
}  // namespace compass::adblock
