// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/web_store/web_store_branding.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {

namespace {

// Shaped like the store's detail page: the label is a <span> in a <button>.
constexpr char kPage[] = R"(<!doctype html>
<button id="add"><span class="x"></span><span>Add to Chrome</span></button>
<button id="it" aria-label="Aggiungi a Chrome"><span>Aggiungi a Chrome</span></button>
<a id="home" href="/">Chrome Web Store</a>
<div id="later"></div>)";

std::unique_ptr<net::test_server::HttpResponse> ServePage(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/html");
  response->set_content(kPage);
  return response;
}

}  // namespace

class WebStoreBrandingBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    net::EmbeddedTestServer::ServerCertificateConfig cert;
    cert.dns_names = {"chromewebstore.google.com", "example.test"};
    https_.SetSSLConfig(cert);
    https_.RegisterRequestHandler(base::BindRepeating(&ServePage));
    ASSERT_TRUE(https_.Start());
  }

  GURL Url(const std::string& host) {
    return https_.GetURL(host, "/detail/some-extension/abcdefghijklmnop");
  }

  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  std::string Text(const std::string& selector) {
    return content::EvalJs(web_contents(), "document.querySelector('" +
                                               selector + "').textContent")
        .ExtractString();
  }

  // Waits until |selector|'s text is |text|.
  bool TextBecomes(const std::string& selector, const std::string& text) {
    return base::test::RunUntil([&] { return Text(selector) == text; });
  }

  net::EmbeddedTestServer https_{net::EmbeddedTestServer::TYPE_HTTPS};
};

IN_PROC_BROWSER_TEST_F(WebStoreBrandingBrowserTest, ButtonsSayCompass) {
  ASSERT_TRUE(IsWebStoreURL(Url("chromewebstore.google.com")));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(),
                                           Url("chromewebstore.google.com")));

  EXPECT_TRUE(TextBecomes("#add", "Add to Compass"));
  EXPECT_TRUE(TextBecomes("#it", "Aggiungi a Compass"));
  EXPECT_EQ("Aggiungi a Compass",
            content::EvalJs(web_contents(),
                            "document.querySelector('#it')"
                            ".getAttribute('aria-label')"));
  // Only buttons.
  EXPECT_EQ("Chrome Web Store", Text("#home"));

  // The store changes the label once the extension is added.
  ASSERT_TRUE(content::ExecJs(
      web_contents(),
      "document.querySelector('#add span:last-child').firstChild.data ="
      " 'Remove from Chrome'"));
  EXPECT_TRUE(TextBecomes("#add", "Remove from Compass"));

  // And renders buttons later (another page in the same document).
  ASSERT_TRUE(content::ExecJs(
      web_contents(),
      "document.querySelector('#later').innerHTML ="
      " '<button id=\"new\"><span>Add to Chrome</span></button>'"));
  EXPECT_TRUE(TextBecomes("#new", "Add to Compass"));
}

IN_PROC_BROWSER_TEST_F(WebStoreBrandingBrowserTest, OtherSitesAreLeftAlone) {
  ASSERT_FALSE(IsWebStoreURL(Url("example.test")));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), Url("example.test")));
  // The script would have run at DOMContentLoaded, before this.
  EXPECT_EQ("Add to Chrome", Text("#add"));
}

}  // namespace compass
