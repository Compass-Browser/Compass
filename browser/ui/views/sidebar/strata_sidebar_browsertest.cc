// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/test/run_until.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/common/tab_strip_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/focus/focus_manager.h"
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
  // The native strip is never built.
  EXPECT_FALSE(FindDescendant<TabStripView>(browser_view()));
  EXPECT_FALSE(region->GetTabStripView());
  EXPECT_GT(sidebar()->height(), 0);
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, CollapsesAndResizes) {
  auto* region = FindDescendant<VerticalTabStripRegionView>(browser_view());
  ASSERT_TRUE(region);
  const int expanded_width = region->width();
  ASSERT_GT(expanded_width, 0);

  // Upstream's resize handle and collapse animation used the native strip.
  region->OnResize(40, /*done_resizing=*/false);
  region->OnResize(40, /*done_resizing=*/true);
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return region->width() > expanded_width; }));

  const int resized_width = region->width();
  region->RequestCollapse(true);
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return region->width() < resized_width; }));
  EXPECT_TRUE(sidebar()->GetVisible());
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

// Drops a link on |selector| in the sidebar, the way dragging one in from a
// page or another app does.
constexpr char kDropLink[] = R"(
  (async (url, selector) => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    let target;
    while (!(target = root.querySelector(selector))) {
      await new Promise(r => setTimeout(r, 20));
    }
    const rect = target.getBoundingClientRect();
    const dataTransfer = new DataTransfer();
    dataTransfer.setData('text/uri-list', url);
    const init = {
      bubbles: true, cancelable: true, composed: true, dataTransfer,
      clientX: rect.left + rect.width / 2,
      clientY: rect.top + rect.height / 2,
    };
    const accepted = !target.dispatchEvent(new DragEvent('dragover', init));
    target.dispatchEvent(new DragEvent('drop', init));
    return accepted;
  })($1, $2))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, DroppingALinkOpensIt) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  content::WebContents* contents = sidebar_contents();

  ASSERT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kDropLink, url, "#pinned")));

  SpaceModel& model = WindowFeatures::From(browser())->space_model();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return model.pinned_root()->children().size() == 1u; }));
  EXPECT_EQ(model.pinned_root()->children()[0]->url(), url);
  TabStripModel* tabs = browser()->tab_strip_model();
  EXPECT_EQ(tabs->count(), 2);
  EXPECT_EQ(tabs->GetActiveWebContents()->GetVisibleURL(), url);
  // The sidebar itself didn't navigate.
  EXPECT_EQ(contents->GetLastCommittedURL(), GURL("chrome://strata-sidebar/"));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, SwitchesSpaces) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  content::WebContents* one = browser()->tab_strip_model()->GetWebContentsAt(0);
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  content::WebContents* contents = sidebar_contents();

  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#addSpace').click();
  )"));

  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  EXPECT_EQ(features->space_model().name(), u"Space 2");
  EXPECT_NE(browser()->tab_strip_model()->GetActiveWebContents(), one);
  EXPECT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      const name = () => root.querySelector('#spaceName')?.textContent.trim();
      while (name() !== 'Space 2' ||
             root.querySelectorAll('.space').length !== 2) {
        await new Promise(r => setTimeout(r, 20));
      }
      return true;
    })())"));
  // Something worth keeping in the new space, so it's parked when we leave.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>Two</title>")));

  // Back to the first space: the same tab, kept alive while hidden. That
  // empties the parking window, and Two goes to a new one.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('.space').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() == home; }));
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), one);

  // And again.
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelectorAll('.space')[1].click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('.space').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() == home; }));
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), one);
}

// Switches |browser|'s window to its space at |index| (0 to 8) with the
// keyboard shortcut. Returns whether the shortcut was taken.
bool PressSpaceShortcut(views::View* browser_view, int index) {
#if BUILDFLAG(IS_MAC)
  constexpr int kModifiers = ui::EF_CONTROL_DOWN;
#else
  constexpr int kModifiers = ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN;
#endif
  return browser_view->GetFocusManager()->ProcessAccelerator(ui::Accelerator(
      static_cast<ui::KeyboardCode>(ui::VKEY_1 + index), kModifiers));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest,
                       DragOntoASpaceMovesTheEntryAndShortcutsSwitch) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>One</title>")));
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<title>Two</title>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* two = browser()->tab_strip_model()->GetActiveWebContents();
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  SpaceModel* work = features->service().space_list().AddSpace(1, u"Work");

  ASSERT_EQ(true, content::EvalJs(sidebar_contents(),
                                  content::JsReplace(kDrag, "Two",
                                                     ".space[title='Work']",
                                                     0.5)));

  // Two moved, with its tab; the window shows One.
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return work->today_root()->children().size() == 1u; }));
  EXPECT_EQ(work->today_root()->children()[0]->title(), u"Two");
  EXPECT_EQ(home->today_root()->children().size(), 1u);
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents()->GetTitle(),
            u"One");

  // Ctrl+2 goes to Work, where Two is still the same page.
  EXPECT_TRUE(PressSpaceShortcut(browser_view(), 1));
  EXPECT_EQ(&features->space_model(), work);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), two);
  EXPECT_TRUE(PressSpaceShortcut(browser_view(), 0));
  EXPECT_EQ(&features->space_model(), home);
  // There's no third space.
  EXPECT_FALSE(PressSpaceShortcut(browser_view(), 2));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, SpaceMenuRenamesColorsDeletes) {
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel* home = &features->space_model();
  content::WebContents* contents = sidebar_contents();
  ASSERT_TRUE(content::ExecJs(contents, R"(
    document.querySelector('strata-sidebar-app').shadowRoot
        .querySelector('#addSpace').click();
  )"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != home; }));
  SpaceModel* added = &features->space_model();

  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      const until = async (f) => {
        while (!f()) await new Promise(r => setTimeout(r, 20));
        return f();
      };
      const button = await until(() => root.querySelectorAll('.space')[1]);
      button.dispatchEvent(new MouseEvent(
          'contextmenu', {bubbles: true, cancelable: true, composed: true}));
      const input = await until(() => root.querySelector('#spaceNameInput'));
      input.value = 'Work';
      input.dispatchEvent(new Event('change'));
      root.querySelector('.swatch[title="Blue"]').click();
      return true;
    })())"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return added->name() == u"Work" && added->color() == SpaceColor::kBlue;
  }));

  // Delete wants a second click. The window goes back to Home.
  ASSERT_EQ(true, content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      const until = async (f) => {
        while (!f()) await new Promise(r => setTimeout(r, 20));
        return f();
      };
      // Reopened from the name, which also starts over.
      root.querySelector('#spaceName').dispatchEvent(new MouseEvent(
          'dblclick', {bubbles: true, composed: true}));
      const remove = await until(() => root.querySelector('#deleteSpace'));
      remove.click();
      await until(() => remove.hasAttribute('confirming'));
      remove.click();
      return true;
    })())"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return features->service().space_list().spaces().size() == 1u;
  }));
  EXPECT_EQ(&features->space_model(), home);
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

// Opens three titled tabs, One to Three, and waits for the sidebar to list
// them. Three is active.
void OpenThreeTabs(BrowserWindowInterface* browser, content::WebContents* sidebar) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser, GURL("data:text/html,<title>One</title>")));
  for (const char* title : {"Two", "Three"}) {
    ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        browser,
        GURL(std::string("data:text/html,<title>") + title + "</title>"),
        WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  }
  ASSERT_EQ(true,
            content::EvalJs(sidebar, content::JsReplace(
                                         kWaitForTitles, base::ListValue()
                                                             .Append("One")
                                                             .Append("Two")
                                                             .Append("Three"))));
}

// Presses |key| on the focused row, and returns the title of the row focused
// after it.
constexpr char kPressKey[] = R"(
  (async (key) => {
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    root.activeElement.dispatchEvent(new KeyboardEvent('keydown', {
      key, bubbles: true, cancelable: true, composed: true,
    }));
    await new Promise(r => setTimeout(r, 0));
    return root.activeElement?.title ?? '';
  })($1))";

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, KeyboardNavigation) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_NO_FATAL_FAILURE(OpenThreeTabs(browser(), contents));
  TabStripModel* tabs = browser()->tab_strip_model();
  // Focus events need a focused page.
  contents->Focus();

  // Focusing the list lands on the active entry.
  EXPECT_EQ("Three", content::EvalJs(contents, R"(
    const root = document.querySelector('strata-sidebar-app').shadowRoot;
    root.querySelector('#sections').focus();
    root.activeElement.title;)"));
  EXPECT_EQ("Two", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "ArrowUp")));
  EXPECT_EQ("One", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "Home")));
  EXPECT_EQ("Two", content::EvalJs(contents,
                                   content::JsReplace(kPressKey, "ArrowDown")));

  ASSERT_TRUE(
      content::ExecJs(contents, content::JsReplace(kPressKey, "Enter")));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return tabs->GetActiveWebContents()->GetTitle() == u"Two";
  }));

  // Closing moves the focus to the next row.
  EXPECT_EQ("Three", content::EvalJs(contents,
                                     content::JsReplace(kPressKey, "Delete")));
  EXPECT_TRUE(base::test::RunUntil([&] { return tabs->count() == 2; }));
}

IN_PROC_BROWSER_TEST_F(StrataSidebarBrowserTest, ArchivePanel) {
  content::WebContents* contents = sidebar_contents();
  ASSERT_NO_FATAL_FAILURE(OpenThreeTabs(browser(), contents));
  WindowFeatures* features = WindowFeatures::From(browser());
  SpaceModel& model = features->space_model();
  // One was last seen two days ago; Three is on screen.
  model.SetLastActive(model.today_root()->children()[0].get(),
                      base::Time::Now() - base::Days(2));

  features->service().ArchiveIdleEntries();

  TabStripModel* tabs = browser()->tab_strip_model();
  ASSERT_TRUE(base::test::RunUntil([&] { return tabs->count() == 2; }));
  ASSERT_EQ(model.archive().size(), 1u);
  EXPECT_EQ(true, content::EvalJs(contents, content::JsReplace(
                                                kWaitForTitles,
                                                base::ListValue()
                                                    .Append("Two")
                                                    .Append("Three"))));

  // The panel lists it; clicking it brings it back.
  EXPECT_EQ("One", content::EvalJs(contents, R"(
    (async () => {
      const root = document.querySelector('strata-sidebar-app').shadowRoot;
      root.querySelector('#archiveButton').click();
      let entry;
      while (!(entry = root.querySelector('.archived'))) {
        await new Promise(r => setTimeout(r, 20));
      }
      const title = entry.title;
      entry.click();
      return title;
    })())"));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return tabs->count() == 3 &&
           tabs->GetActiveWebContents()->GetTitle() == u"One";
  }));
  EXPECT_TRUE(model.archive().empty());
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
