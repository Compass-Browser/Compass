// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/test/run_until.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/devtools/devtools_window_testing.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/dev_mode/dev_mode.h"
#include "compass/browser/dev_mode/network_log.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/views/dev_mode/dev_mode_bar.h"
#include "compass/browser/ui/views/dev_mode/dev_mode_controller.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/controls/textfield/textfield.h"

namespace compass {
namespace {

class DevModeBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestMonitor(base::BindRepeating(
        &DevModeBrowserTest::OnRequest, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());
  }

 protected:
  PrefService* prefs() { return browser()->GetProfile()->GetPrefs(); }
  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
  ContentsContainerView* pane() {
    return BrowserView::GetBrowserViewForBrowser(browser())
        ->multi_contents_view()
        ->contents_container_views()[0];
  }
  DevModeBar* bar() { return DevModeBar::FindIn(pane()); }

  // The Cache-Control header of the last request for |path|.
  std::string LastCacheControl(const std::string& path) {
    base::AutoLock lock(lock_);
    for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
      if (it->first == path) {
        return it->second;
      }
    }
    return "<none>";
  }
  size_t RequestCount(const std::string& path) {
    base::AutoLock lock(lock_);
    size_t count = 0;
    for (const auto& [p, cache] : requests_) {
      count += p == path;
    }
    return count;
  }

 private:
  void OnRequest(const net::test_server::HttpRequest& request) {
    base::AutoLock lock(lock_);
    auto it = request.headers.find("Cache-Control");
    requests_.emplace_back(request.relative_url,
                           it == request.headers.end() ? "" : it->second);
  }

  base::Lock lock_;
  std::vector<std::pair<std::string, std::string>> requests_;
};

// A page on a site in development mode gets the bar, which takes the top of
// the pane; the page leaving the list drops it.
IN_PROC_BROWSER_TEST_F(DevModeBrowserTest, BarTakesTheTopOfThePane) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  EXPECT_FALSE(bar());
  const int page_top = pane()->contents_view()->y();

  ASSERT_TRUE(dev_mode::AddHost(prefs(), "127.0.0.1"));
  ASSERT_TRUE(base::test::RunUntil([&] { return bar() != nullptr; }));
  EXPECT_EQ(bar()->url_field_for_testing()->GetText(),
            base::UTF8ToUTF16(url.spec()));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return pane()->contents_view()->y() == page_top + DevModeBar::kHeight;
  }));
  EXPECT_EQ(bar()->bounds().bottom(), pane()->contents_view()->y());

  // Another page on the site: the URL follows.
  const GURL other = embedded_test_server()->GetURL("/title2.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), other));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return bar() && bar()->url_field_for_testing()->GetText() ==
                        base::UTF8ToUTF16(other.spec());
  }));

  dev_mode::RemoveHost(prefs(), "127.0.0.1");
  ASSERT_TRUE(base::test::RunUntil([&] { return bar() == nullptr; }));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return pane()->contents_view()->y() == page_top; }));
}

// The URL can be edited: Enter goes there, a javascript: URL doesn't run,
// Esc puts the page's URL back.
IN_PROC_BROWSER_TEST_F(DevModeBrowserTest, EditingTheUrlNavigates) {
  ASSERT_TRUE(dev_mode::AddHost(prefs(), "127.0.0.1"));
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  ASSERT_TRUE(base::test::RunUntil([&] { return bar() != nullptr; }));
  views::Textfield* field = bar()->url_field_for_testing();
  const ui::KeyEvent enter(ui::EventType::kKeyPressed, ui::VKEY_RETURN,
                           ui::EF_NONE);
  const ui::KeyEvent escape(ui::EventType::kKeyPressed, ui::VKEY_ESCAPE,
                            ui::EF_NONE);

  field->SetText(u"javascript:document.title='ran'");
  EXPECT_TRUE(bar()->HandleKeyEvent(field, enter));
  EXPECT_NE(contents()->GetTitle(), u"ran");
  EXPECT_EQ(field->GetText(), base::UTF8ToUTF16(url.spec()));

  field->SetText(u"somewhere typed");
  EXPECT_TRUE(bar()->HandleKeyEvent(field, escape));
  EXPECT_EQ(field->GetText(), base::UTF8ToUTF16(url.spec()));

  const GURL other = embedded_test_server()->GetURL("/title2.html");
  field->SetText(base::UTF8ToUTF16(other.spec()));
  content::TestNavigationObserver observer(contents());
  EXPECT_TRUE(bar()->HandleKeyEvent(field, enter));
  observer.Wait();
  EXPECT_EQ(contents()->GetLastCommittedURL(), other);
}

// The shortcut's command turns it on and off for the page's site.
IN_PROC_BROWSER_TEST_F(DevModeBrowserTest, CommandToggles) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  chrome::ExecuteCommand(browser(), kToggleDevModeCommand);
  EXPECT_TRUE(dev_mode::IsEnabledFor(prefs(), url));
  ASSERT_TRUE(base::test::RunUntil([&] { return bar() != nullptr; }));
  chrome::ExecuteCommand(browser(), kToggleDevModeCommand);
  EXPECT_FALSE(dev_mode::IsEnabledFor(prefs(), url));
  ASSERT_TRUE(base::test::RunUntil([&] { return bar() == nullptr; }));
}

// Reload skips the cache on a site in development mode, and only there.
IN_PROC_BROWSER_TEST_F(DevModeBrowserTest, ReloadSkipsTheCache) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));

  chrome::Reload(browser(), WindowOpenDisposition::CURRENT_TAB);
  EXPECT_TRUE(content::WaitForLoadStop(contents()));
  EXPECT_NE(LastCacheControl("/title1.html"), "no-cache");

  ASSERT_TRUE(dev_mode::AddHost(prefs(), "127.0.0.1"));
  const size_t before = RequestCount("/title1.html");
  chrome::Reload(browser(), WindowOpenDisposition::CURRENT_TAB);
  EXPECT_TRUE(content::WaitForLoadStop(contents()));
  ASSERT_GT(RequestCount("/title1.html"), before);
  EXPECT_EQ(LastCacheControl("/title1.html"), "no-cache");
}

// The page's requests from before DevTools opened are in its Network panel.
IN_PROC_BROWSER_TEST_F(DevModeBrowserTest, NetworkPanelHasEarlierRequests) {
  ASSERT_TRUE(dev_mode::AddHost(prefs(), "127.0.0.1"));
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  // A request after the load, too.
  ASSERT_TRUE(content::ExecJs(contents(),
                              "fetch('/title2.html').then(r => r.text())"));
  dev_mode::DevModeTabHelper* helper =
      dev_mode::DevModeTabHelper::FromWebContents(contents());
  ASSERT_TRUE(helper && helper->network_log());
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return helper->network_log()->buffer().request_count() >= 2; }));

  DevToolsWindow* window =
      DevToolsWindowTesting::OpenDevToolsWindowSync(contents(), true);
  content::WebContents* frontend =
      DevToolsWindowTesting::Get(window)->main_web_contents();
  // The frontend's own log of requests.
  const std::string urls = R"(
    (async () => {
      const Logs = await import('./models/logs/logs.js');
      for (let i = 0; i < 100; i++) {
        const urls = Logs.NetworkLog.NetworkLog.instance().requests()
            .map(r => r.url());
        if (urls.some(u => u.endsWith('/title2.html'))) {
          return urls.join(' ');
        }
        await new Promise(r => setTimeout(r, 50));
      }
      return 'none: ' + Logs.NetworkLog.NetworkLog.instance().requests()
          .map(r => r.url()).join(' ');
    })())";
  const std::string seen = content::EvalJs(frontend, urls).ExtractString();
  EXPECT_NE(seen.find(url.spec()), std::string::npos) << seen;
  EXPECT_NE(seen.find("/title2.html"), std::string::npos) << seen;

  // A replayed request's body comes from the log's session.
  const std::string body = R"(
    (async () => {
      const Logs = await import('./models/logs/logs.js');
      const request = Logs.NetworkLog.NetworkLog.instance().requests()
          .find(r => r.url().endsWith('/title2.html'));
      const content = await request.requestContentData();
      return content.text ?? content.error ?? String(content);
    })())";
  EXPECT_NE(content::EvalJs(frontend, body).ExtractString().find("Title Of"),
            std::string::npos);

  DevToolsWindowTesting::CloseDevToolsWindowSync(window);
}

}  // namespace
}  // namespace compass
