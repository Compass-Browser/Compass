// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/zap.h"

#include <memory>
#include <string>
#include <vector>

#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/boosts/boost_injector.h"
#include "compass/browser/boosts/boost_store.h"
#include "compass/browser/ui/zap/zap_session.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "url/gurl.h"

namespace compass::zap {
namespace {

constexpr char kPage[] =
    "<!doctype html><title>Zap</title><body style='margin:0'>"
    "<div id='banner' style='height:100px;background:red'>Ad</div>"
    "<main style='height:100px'>Article</main></body>";

class ZapBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        [](const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_content_type("text/html");
          response->set_content(kPage);
          return response;
        }));
    ASSERT_TRUE(embedded_test_server()->Start());
    base::test::TestFuture<void> loaded;
    store()->WhenLoaded(loaded.GetCallback());
    ASSERT_TRUE(loaded.Wait());
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("zap.test", "/page.html")));
  }

  boosts::BoostStore* store() {
    return boosts::BoostStore::Get(browser()->GetProfile());
  }
  content::WebContents* tab() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  bool BannerShows() {
    return content::EvalJs(tab(),
                           "getComputedStyle(document.getElementById('banner'))"
                           ".display !== 'none'")
        .ExtractBool();
  }

  void Click(int x, int y) {
    using blink::WebInputEvent;
    const gfx::Point point(x, y);
    content::SimulateMouseEvent(tab(), WebInputEvent::Type::kMouseMove,
                                blink::WebMouseEvent::Button::kNoButton, point);
    content::SimulateMouseClickAt(tab(), 0, blink::WebMouseEvent::Button::kLeft,
                                  point);
  }
};

// Clicking an element hides it; Done keeps it hidden on the site with a zap
// Boost, which applies after a reload.
IN_PROC_BROWSER_TEST_F(ZapBrowserTest, ClickedElementStaysHidden) {
  ASSERT_EQ(ZapSiteFor(tab()), "zap.test");
  StartZap(tab());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(tab(), "!!document.querySelector('[popover]')")
        .ExtractBool();
  }));
  Click(50, 50);
  ASSERT_TRUE(base::test::RunUntil([&] { return !BannerShows(); }));

  // Enter is Done.
  content::SimulateKeyPress(tab(), ui::DomKey::ENTER, ui::DomCode::ENTER,
                            ui::VKEY_RETURN, false, false, false, false);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return boosts::FindZapBoost(*store(), "zap.test") != nullptr; }));
  const boosts::Boost* boost = boosts::FindZapBoost(*store(), "zap.test");
  EXPECT_EQ(boosts::ZappedSelectors(*boost),
            (std::vector<std::string>{"#banner"}));

  base::test::TestFuture<void> registered;
  boosts::BoostInjector::Get(browser()->GetProfile())
      ->WhenRegistered(registered.GetCallback());
  ASSERT_TRUE(registered.Wait());
  content::TestNavigationObserver reload(tab());
  tab()->GetController().Reload(content::ReloadType::NORMAL, false);
  reload.Wait();
  EXPECT_FALSE(BannerShows());
}

// Esc puts back what was hidden and keeps nothing.
IN_PROC_BROWSER_TEST_F(ZapBrowserTest, EscapeRestores) {
  StartZap(tab());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(tab(), "!!document.querySelector('[popover]')")
        .ExtractBool();
  }));
  Click(50, 50);
  ASSERT_TRUE(base::test::RunUntil([&] { return !BannerShows(); }));
  content::SimulateKeyPress(tab(), ui::DomKey::ESCAPE, ui::DomCode::ESCAPE,
                            ui::VKEY_ESCAPE, false, false, false, false);
  ASSERT_TRUE(base::test::RunUntil([&] { return BannerShows(); }));
  EXPECT_FALSE(boosts::FindZapBoost(*store(), "zap.test"));
}

}  // namespace
}  // namespace compass::zap
