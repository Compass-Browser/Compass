// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/run_until.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace strata {
namespace {

template <typename T>
T* FindDescendant(views::View* root) {
  if (auto* match = views::AsViewClass<T>(root)) {
    return match;
  }
  for (views::View* child : root->children()) {
    if (T* match = FindDescendant<T>(child)) {
      return match;
    }
  }
  return nullptr;
}

// Waits until the sidebar page has rendered |count| tab rows.
constexpr char kWaitForTabRows[] = R"(
  (async (count) => {
    const rows = () => document.querySelector('strata-sidebar-app')
        ?.shadowRoot?.querySelectorAll('.tab') ?? [];
    while (rows().length !== count) {
      await new Promise(r => setTimeout(r, 20));
    }
    return [...rows()].map(r => r.title);
  })($1))";

class StrataSidebarBrowserTest : public InProcessBrowserTest {
 protected:
  views::View* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }

  StrataSidebarView* sidebar() {
    return FindDescendant<StrataSidebarView>(browser_view());
  }

  content::WebContents* sidebar_contents() {
    content::WebContents* contents = sidebar()->GetWebContents();
    EXPECT_TRUE(content::WaitForLoadStop(contents));
    return contents;
  }
};

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ReplacesNativeTabStrip) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  ASSERT_TRUE(sidebar());
  EXPECT_TRUE(region->Contains(sidebar()));
  EXPECT_TRUE(sidebar()->GetVisible());
  EXPECT_EQ(sidebar()->GetWebContents()->GetVisibleURL(),
            GURL("chrome://strata-sidebar/"));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ListsTabs) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));

  EXPECT_EQ(browser()->tab_strip_model()->count(), 2);
  EXPECT_EQ(content::EvalJs(sidebar_contents(),
                            content::JsReplace(kWaitForTabRows, 2))
                .ExtractList()
                .size(),
            2u);
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ClickingATabActivatesIt) {
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_EQ(model->active_index(), 1);
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kWaitForTabRows, 2)));

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelectorAll('.tab')[0].click();)"));

  EXPECT_TRUE(base::test::RunUntil([&] { return model->active_index() == 0; }));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, NewTabButtonOpensATab) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kWaitForTabRows, 1)));

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#newTab').click();)"));

  EXPECT_TRUE(base::test::RunUntil(
      [&] { return browser()->tab_strip_model()->count() == 2; }));
}

// Waits until the sidebar's tab rows have the titles |titles|, in order.
constexpr char kWaitForTitles[] = R"(
  (async (titles) => {
    const current = () => [...(document.querySelector('strata-sidebar-app')
        ?.shadowRoot?.querySelectorAll('.tab') ?? [])].map(r => r.title);
    while (JSON.stringify(current()) !== JSON.stringify(titles)) {
      await new Promise(r => setTimeout(r, 20));
    }
    return true;
  })($1))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest,
                       ModelMovesReorderSidebarAndStrip) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Two</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(
                              kWaitForTitles,
                              base::ListValue().Append("One").Append("Two"))));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  model.Move(model.today_root()->children()[1].get(), model.today_root(), 0);

  EXPECT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(
                              kWaitForTitles,
                              base::ListValue().Append("Two").Append("One"))));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return browser()->tab_strip_model()->GetWebContentsAt(0)->GetTitle() ==
           u"Two";
  }));
}

// Drags the entry titled $1 onto $2, a CSS selector in the sidebar's shadow
// root, at fraction $3 of the target's height, using synthetic drag events.
constexpr char kDrag[] = R"(
  (async (title, selector, fraction) => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    const until = async (f) => {
      while (!f()) await new Promise(r => setTimeout(r, 20));
      return f();
    };
    const row = await until(() => [...root.querySelectorAll('.row')]
        .find(r => r.title === title || r.textContent.trim() === title));
    const target = await until(() => root.querySelector(selector));
    const rect = target.getBoundingClientRect();
    const init = {
      bubbles: true, cancelable: true, composed: true,
      dataTransfer: new DataTransfer(),
      clientX: rect.left + rect.width / 2,
      clientY: rect.top + rect.height * fraction,
    };
    row.dispatchEvent(new DragEvent('dragstart', init));
    target.dispatchEvent(new DragEvent('dragover', init));
    target.dispatchEvent(new DragEvent('drop', init));
    row.dispatchEvent(new DragEvent('dragend', init));
    return true;
  })($1, $2, $3))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, DragToPinnedPinsATab) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Two</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(
                              kWaitForTitles,
                              base::ListValue().Append("One").Append("Two"))));

  ASSERT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kDrag, "Two", "#pinned", 0.5)));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return model.pinned_root()->children().size() == 1u; }));
  EXPECT_EQ(model.pinned_root()->children()[0]->title(), u"Two");
  EXPECT_TRUE(base::test::RunUntil([&] {
    return browser()->tab_strip_model()->GetWebContentsAt(0)->GetTitle() ==
           u"Two";
  }));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, DragIntoAFolder) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* contents = sidebar_contents();
  ASSERT_EQ(true,
            content::EvalJs(
                contents, content::JsReplace(kWaitForTitles,
                                             base::ListValue().Append("One"))));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#newFolder').click();)"));

  ASSERT_EQ(true,
            content::EvalJs(contents, content::JsReplace(kDrag, "One",
                                                         ".folder-row", 0.5)));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return model.pinned_root()->children().size() == 1u &&
           model.pinned_root()->children()[0]->children().size() == 1u;
  }));
  const SidebarNode* folder = model.pinned_root()->children()[0].get();
  EXPECT_EQ(folder->type(), SidebarNode::Type::kFolder);
  EXPECT_EQ(folder->children()[0]->title(), u"One");
}

// Window sync across real windows (docs/design/sidebar.md, "Window sync").
using StrataWindowSyncBrowserTest = StrataSidebarBrowserTest;

IN_PROC_BROWSER_TEST_F(StrataWindowSyncBrowserTest,
                       ClosingAWindowKeepsItsTabs) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      second, GURL("data:text/html,<title>Two</title>")));
  content::WebContents* two =
      second->GetTabStripModel()->GetActiveWebContents();

  CloseBrowserSynchronously(second);

  // The same page, moved, not reloaded.
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_EQ(model->count(), 2);
  EXPECT_NE(model->GetIndexOfWebContents(two), TabStripModel::kNoTab);
  EXPECT_EQ(model->GetActiveWebContents()->GetTitle(), u"One");
}

IN_PROC_BROWSER_TEST_F(StrataWindowSyncBrowserTest,
                       PlaceholderBringsTheTabBack) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  BrowserWindowInterface* second = CreateBrowser(browser()->GetProfile());
  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  SidebarNode* one = nullptr;
  ASSERT_TRUE(base::test::RunUntil([&] {
    for (const auto& node : model.today_root()->children()) {
      if (node->title() == u"One") {
        one = node.get();
      }
    }
    return one != nullptr;
  }));

  // Open One from the second window: it moves there, and the first window,
  // which has no other tab, shows a placeholder instead of closing.
  WindowFeatures::From(second)->tab_strip_bridge().OpenNode(one);
  ASSERT_EQ(browser()->tab_strip_model()->count(), 1);
  content::WebContents* placeholder =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(placeholder));
  EXPECT_EQ(true, content::EvalJs(placeholder, R"(
    (async () => {
      while (document.querySelector('#title').textContent !== 'One') {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));

  // It shows the tab as it was when it left.
  EXPECT_EQ(true, content::EvalJs(placeholder, R"(
    (async () => {
      const img = document.querySelector('#snapshot');
      while (!img.src) {
        await new Promise(r => setTimeout(r, 20));
      }
      await img.decode();
      return img.src.startsWith('data:image/jpeg') && img.naturalWidth > 0;
    })())"));

  ASSERT_TRUE(content::ExecJs(placeholder,
                              "document.querySelector('#bringHere').click()"));

  EXPECT_TRUE(base::test::RunUntil([&] {
    TabStripModel* first = browser()->tab_strip_model();
    return first->count() == 1 &&
           first->GetActiveWebContents()->GetTitle() == u"One";
  }));
  // Now the second window keeps showing One, as a placeholder.
  EXPECT_EQ(second->GetTabStripModel()->count(), 2);
}

}  // namespace
}  // namespace strata
