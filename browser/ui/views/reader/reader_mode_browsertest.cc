// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/reader/reader_mode.h"

#include <string>

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/app/chrome_command_ids.h"
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
#include "components/input/native_web_keyboard_event.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/views/reader/reader_tab_state.h"
#include "compass/browser/ui/views/sidebar/sidebar_header.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/test/button_test_api.h"

namespace compass {
namespace {

constexpr char kArticleTitle[] = "The Lighthouse Keepers of Capo Rosso";

class ReaderModeBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    // common/reader/: the saved pages and the shared test.
    base::FilePath root;
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
    embedded_test_server()->ServeFilesFromDirectory(
        root.AppendASCII("compass").AppendASCII("common").AppendASCII(
            "reader"));
    ASSERT_TRUE(embedded_test_server()->Start());
  }

 protected:
  GURL ArticleUrl() {
    return embedded_test_server()->GetURL("/test/fixtures/article.html");
  }
  GURL NotArticleUrl() {
    return embedded_test_server()->GetURL("/test/fixtures/not_article.html");
  }

  content::WebContents* active_contents(BrowserWindowInterface* window) {
    return window->GetTabStripModel()->GetActiveWebContents();
  }
  content::WebContents* active_contents() {
    return active_contents(browser());
  }
  ReaderTabState* state(content::WebContents* contents) {
    return ReaderTabState::FromWebContents(contents);
  }
  views::ImageButton* reader_button(BrowserWindowInterface* window) {
    SidebarHeader* header = SidebarHeader::From(window);
    return header ? header->reader_button_for_testing() : nullptr;
  }

  // Loads |url| and waits until Reader has decided whether it's an article.
  void Load(BrowserWindowInterface* window, const GURL& url) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(window, url));
    content::WebContents* contents = active_contents(window);
    ASSERT_TRUE(base::test::RunUntil([&] {
      return state(contents) && state(contents)->checked_for_testing();
    }));
  }

  // Waits for the reader view of |contents| to show its article.
  content::WebContents* WaitForReaderView(content::WebContents* contents) {
    EXPECT_TRUE(base::test::RunUntil([&] {
      return state(contents)->showing() &&
             state(contents)->reader_contents_for_testing() &&
             state(contents)->view_for_testing();
    }));
    content::WebContents* reader =
        state(contents)->reader_contents_for_testing();
    EXPECT_TRUE(content::WaitForLoadStop(reader));
    EXPECT_TRUE(base::test::RunUntil([&] {
      return content::EvalJs(reader, "document.body.dataset.state || ''")
                 .ExtractString() != "";
    }));
    return reader;
  }

  // As the renderer host does with a key the page didn't handle.
  void PressEscapeIn(content::WebContents* reader) {
    input::NativeWebKeyboardEvent escape(
        blink::WebInputEvent::Type::kRawKeyDown,
        blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
    escape.windows_key_code = ui::VKEY_ESCAPE;
    EXPECT_TRUE(reader->GetDelegate()->HandleKeyboardEvent(reader, escape));
  }
};

// common/reader/test/reader_test.html: reader.js on the saved pages and the
// sanitizer, in Compass's own engine.
IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, SharedTests) {
  const GURL url = embedded_test_server()->GetURL("/test/reader_test.html");
  content::TitleWatcher watcher(active_contents(), u"PASS");
  watcher.AlsoWaitForTitle(u"FAIL");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
  const std::u16string title = watcher.WaitAndGetTitle();
  EXPECT_EQ(title, u"PASS") << content::EvalJs(
                                  active_contents(),
                                  "document.getElementById('log').textContent")
                                  .ExtractString();
}

IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, ArticleShowsReader) {
  ASSERT_NO_FATAL_FAILURE(Load(browser(), ArticleUrl()));
  content::WebContents* tab = active_contents();
  ReaderMode* reader_mode = ReaderMode::From(browser());
  ASSERT_TRUE(reader_mode);
  EXPECT_TRUE(reader_mode->IsAvailable());
  EXPECT_FALSE(reader_mode->IsShowing());
  views::ImageButton* button = reader_button(browser());
  ASSERT_TRUE(button);
  EXPECT_TRUE(button->GetVisible());
  EXPECT_EQ(button->GetTooltipText(), u"Show Reader");

  const int entries = tab->GetController().GetEntryCount();
  const GURL url = tab->GetLastCommittedURL();

  // The button enters Reader.
  views::test::ButtonTestApi(button).NotifyClick(ui::MouseEvent(
      ui::EventType::kMousePressed, gfx::Point(), gfx::Point(),
      ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON, 0));
  content::WebContents* reader = WaitForReaderView(tab);
  EXPECT_EQ(content::EvalJs(reader, "document.body.dataset.state"), "ready");
  EXPECT_EQ(content::EvalJs(reader,
                            "document.getElementById('title').textContent"),
            kArticleTitle);
  EXPECT_EQ(content::EvalJs(reader,
                            "document.getElementById('site').textContent"),
            "Coastal Notes");
  const std::string html =
      content::EvalJs(reader, "document.getElementById('content').innerHTML")
          .ExtractString();
  EXPECT_NE(html.find("Rosa Bellini"), std::string::npos) << html;
  EXPECT_EQ(html.find("<script"), std::string::npos) << html;
  EXPECT_EQ(html.find("onclick"), std::string::npos) << html;
  EXPECT_EQ(html.find("<iframe"), std::string::npos) << html;
  EXPECT_EQ(html.find("<form"), std::string::npos) << html;
  EXPECT_EQ(reader->GetLastCommittedURL().scheme(), "chrome-untrusted");
  EXPECT_TRUE(reader_mode->IsShowing());
  EXPECT_EQ(button->GetTooltipText(), u"Hide Reader");

  // Over the tab's pane; the tab is as it was: same URL, same history, the
  // same document with its scripts' state.
  views::WebView* view = state(tab)->view_for_testing();
  ContentsContainerView* pane =
      BrowserView::GetBrowserViewForBrowser(browser())
          ->multi_contents_view()
          ->GetContentsContainerViewFor(tab);
  ASSERT_TRUE(pane);
  EXPECT_EQ(view->parent(), pane);
  EXPECT_TRUE(view->IsDrawn());
  EXPECT_EQ(view->bounds(), pane->contents_view()->bounds());
  EXPECT_EQ(tab->GetLastCommittedURL(), url);
  EXPECT_EQ(tab->GetController().GetEntryCount(), entries);
  EXPECT_EQ(content::EvalJs(tab, "window.pageScriptRan === true"), true);
  EXPECT_EQ(active_contents(), tab);

  // Esc leaves; the page is still there.
  PressEscapeIn(reader);
  EXPECT_TRUE(base::test::RunUntil([&] { return !state(tab)->showing(); }));
  EXPECT_FALSE(state(tab)->view_for_testing());
  EXPECT_FALSE(state(tab)->reader_contents_for_testing());
  EXPECT_EQ(tab->GetLastCommittedURL(), url);
  EXPECT_EQ(tab->GetController().GetEntryCount(), entries);
  EXPECT_EQ(content::EvalJs(tab, "window.pageScriptRan === true"), true);
  EXPECT_TRUE(button->GetVisible());
  EXPECT_EQ(button->GetTooltipText(), u"Show Reader");
}

IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, NotAnArticle) {
  ASSERT_NO_FATAL_FAILURE(Load(browser(), NotArticleUrl()));
  EXPECT_FALSE(state(active_contents())->readerable());
  EXPECT_FALSE(ReaderMode::From(browser())->IsAvailable());
  ASSERT_TRUE(reader_button(browser()));
  EXPECT_FALSE(reader_button(browser())->GetVisible());

  // From an article to a page that isn't, the button goes.
  ASSERT_NO_FATAL_FAILURE(Load(browser(), ArticleUrl()));
  EXPECT_TRUE(reader_button(browser())->GetVisible());
  ASSERT_NO_FATAL_FAILURE(Load(browser(), NotArticleUrl()));
  EXPECT_FALSE(reader_button(browser())->GetVisible());

  // The command still enters: the view shows what Readability found, or
  // says there's no article.
  chrome::ExecuteCommand(browser(), kToggleReaderModeCommand);
  WaitForReaderView(active_contents());
}

// A new document in the tab leaves Reader.
IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, NavigationLeaves) {
  ASSERT_NO_FATAL_FAILURE(Load(browser(), ArticleUrl()));
  content::WebContents* tab = active_contents();
  ReaderMode::From(browser())->Toggle();
  WaitForReaderView(tab);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), NotArticleUrl()));
  EXPECT_FALSE(state(tab)->showing());
  EXPECT_FALSE(state(tab)->view_for_testing());
}

// A plain click on a link in the article loads it in the tab.
IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, LinkLoadsInTheTab) {
  ASSERT_NO_FATAL_FAILURE(Load(browser(), ArticleUrl()));
  content::WebContents* tab = active_contents();
  ReaderMode::From(browser())->Toggle();
  content::WebContents* reader = WaitForReaderView(tab);
  content::TestNavigationObserver observer(tab);
  ASSERT_TRUE(content::ExecJs(
      reader,
      "[...document.querySelectorAll('#content a')].find("
      "  (a) => a.textContent === 'our archive').click();"));
  observer.Wait();
  EXPECT_EQ(tab->GetLastCommittedURL().path(), "/archive/lighthouses");
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  EXPECT_FALSE(state(tab)->showing());
}

// Reader is per tab: another tab hides the view, coming back shows it.
IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, TabSwitchKeepsReader) {
  ASSERT_NO_FATAL_FAILURE(Load(browser(), ArticleUrl()));
  content::WebContents* tab = active_contents();
  ReaderMode::From(browser())->Toggle();
  WaitForReaderView(tab);
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), NotArticleUrl(), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_NE(active_contents(), tab);
  EXPECT_TRUE(
      base::test::RunUntil([&] { return !state(tab)->view_for_testing(); }));
  EXPECT_TRUE(state(tab)->showing());
  EXPECT_FALSE(ReaderMode::From(browser())->IsShowing());

  TabStripModel* tabs = browser()->tab_strip_model();
  tabs->ActivateTabAt(tabs->GetIndexOfWebContents(tab));
  EXPECT_TRUE(
      base::test::RunUntil([&] { return state(tab)->view_for_testing(); }));
  EXPECT_TRUE(ReaderMode::From(browser())->IsShowing());
}

// The Reader command and upstream's Reading mode commands toggle it.
IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, Commands) {
  ASSERT_NO_FATAL_FAILURE(Load(browser(), ArticleUrl()));
  content::WebContents* tab = active_contents();
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleReaderModeCommand));
  WaitForReaderView(tab);
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), IDC_SHOW_READING_MODE));
  EXPECT_FALSE(state(tab)->showing());
  EXPECT_TRUE(
      chrome::ExecuteCommand(browser(), IDC_SHOW_READING_MODE_KEYBOARD));
  WaitForReaderView(tab);
  EXPECT_TRUE(chrome::ExecuteCommand(browser(), kToggleReaderModeCommand));
  EXPECT_FALSE(state(tab)->showing());
}

// The view's settings stay for the next article.
IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, SettingsAreRemembered) {
  ASSERT_NO_FATAL_FAILURE(Load(browser(), ArticleUrl()));
  content::WebContents* tab = active_contents();
  ReaderMode::From(browser())->Toggle();
  content::WebContents* reader = WaitForReaderView(tab);
  EXPECT_EQ(content::EvalJs(reader, "document.documentElement.dataset.theme"),
            "auto");
  ASSERT_TRUE(content::ExecJs(
      reader, "document.querySelector('[data-value=sepia]').click();"
              "document.querySelector('[data-value=sans]').click();"));
  ReaderMode::From(browser())->Toggle();
  ASSERT_FALSE(state(tab)->showing());

  ReaderMode::From(browser())->Toggle();
  reader = WaitForReaderView(tab);
  EXPECT_EQ(content::EvalJs(reader, "document.documentElement.dataset.theme"),
            "sepia");
  EXPECT_EQ(content::EvalJs(reader, "document.documentElement.dataset.font"),
            "sans");
}

IN_PROC_BROWSER_TEST_F(ReaderModeBrowserTest, Incognito) {
  BrowserWindowInterface* incognito = CreateIncognitoBrowser();
  ASSERT_TRUE(incognito->GetProfile()->IsOffTheRecord());
  ASSERT_NO_FATAL_FAILURE(Load(incognito, ArticleUrl()));
  content::WebContents* tab = active_contents(incognito);
  ReaderMode* reader_mode = ReaderMode::From(incognito);
  ASSERT_TRUE(reader_mode);
  EXPECT_TRUE(reader_mode->IsAvailable());
  reader_mode->Toggle();
  content::WebContents* reader = WaitForReaderView(tab);
  EXPECT_EQ(reader->GetBrowserContext(), tab->GetBrowserContext());
  EXPECT_EQ(content::EvalJs(reader,
                            "document.getElementById('title').textContent"),
            kArticleTitle);
}

}  // namespace
}  // namespace compass
