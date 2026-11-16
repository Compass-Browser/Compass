// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <optional>
#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/boosts/boost_share.h"
#include "compass/browser/boosts/boost_store.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "url/gurl.h"

namespace compass::boosts {
namespace {

// Finds |selector| anywhere in the page, through every shadow root
// (chrome://settings nests the sections in settings-ui and settings-main).
constexpr char kDeepQuery[] = R"(
  function deepQuery(selector, root = document) {
    const found = root.querySelector(selector);
    if (found) {
      return found;
    }
    for (const element of root.querySelectorAll('*')) {
      if (element.shadowRoot) {
        const inside = deepQuery(selector, element.shadowRoot);
        if (inside) {
          return inside;
        }
      }
    }
    return null;
  }
)";

constexpr char kPreview[] = "chrome://settings/boosts?import=";

class BoostShareBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    ASSERT_TRUE(base::test::RunUntil([&] { return store()->loaded(); }));
  }

  BoostStore* store() { return BoostStore::Get(browser()->GetProfile()); }
  TabStripModel* tabs() { return browser()->tab_strip_model(); }

  static Boost MakeBoost() {
    Boost boost;
    boost.name = "Shared colors";
    boost.match = "a.test";
    boost.css = "body { color: rgb(1, 2, 3); }";
    boost.js = "document.title = 'boosted';";
    boost.js_world = kMainWorld;
    return boost;
  }

  // Resolves with what |expression| (using deepQuery()) gives once it's
  // truthy.
  static content::EvalJsResult WaitFor(content::WebContents* contents,
                                       const std::string& expression) {
    return content::EvalJs(contents, std::string(kDeepQuery) + R"(
      new Promise(resolve => {
        const check = () => {
          const value = ()" + expression + R"();
          if (value) {
            resolve(value);
          } else {
            setTimeout(check, 20);
          }
        };
        check();
      }))");
  }

  // The tab showing the import preview, once there is one in |in|.
  static content::WebContents* WaitForPreview(BrowserWindowInterface* in) {
    content::WebContents* found = nullptr;
    EXPECT_TRUE(base::test::RunUntil([&] {
      for (int i = 0; i < in->tab_strip_model()->count(); ++i) {
        content::WebContents* contents =
            in->tab_strip_model()->GetWebContentsAt(i);
        if (contents->GetVisibleURL().spec().starts_with(kPreview)) {
          found = contents;
          return true;
        }
      }
      return false;
    }));
    if (found) {
      EXPECT_TRUE(content::WaitForLoadStop(found));
    }
    return found;
  }
};

// A link in a page opens the preview in a new tab and leaves the page
// alone; confirming adds the Boost as a new, shared one, every time.
IN_PROC_BROWSER_TEST_F(BoostShareBrowserTest, LinkInAPageOpensThePreview) {
  const GURL page = embedded_test_server()->GetURL("a.test", "/empty.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), page));
  content::WebContents* original = tabs()->GetActiveWebContents();
  const std::string link = ShareLink(MakeBoost());
  ASSERT_TRUE(content::ExecJs(original, "location.href = '" + link + "';"));

  content::WebContents* preview = WaitForPreview(browser());
  ASSERT_TRUE(preview);
  EXPECT_NE(preview, original);
  EXPECT_EQ(preview->GetVisibleURL().spec(),
            kPreview + EncodeShareFragment(MakeBoost()));
  EXPECT_EQ(original->GetLastCommittedURL(), page);
  EXPECT_EQ(tabs()->GetActiveWebContents(), preview);

  // The preview shows it all, and warns about the JavaScript.
  EXPECT_EQ(WaitFor(preview,
                    "deepQuery('compass-boosts-page #boostImport')?.hidden "
                    "=== false && deepQuery('compass-boosts-page "
                    "#importName')?.textContent"),
            "Shared colors");
  EXPECT_EQ(content::EvalJs(preview, std::string(kDeepQuery) +
                                         "deepQuery('compass-boosts-page "
                                         "#importMatch').textContent"),
            "a.test");
  EXPECT_EQ(content::EvalJs(preview, std::string(kDeepQuery) +
                                         "deepQuery('compass-boosts-page "
                                         "#importJs').textContent"),
            MakeBoost().js);
  EXPECT_EQ(content::EvalJs(preview, std::string(kDeepQuery) +
                                         "deepQuery('compass-boosts-page "
                                         "#importWarning').textContent"),
            "This Boost runs JavaScript on a.test. Only add it if you trust "
            "whoever shared it.");
  EXPECT_TRUE(store()->boosts().empty());

  ASSERT_TRUE(content::ExecJs(preview, std::string(kDeepQuery) +
                                           "deepQuery('compass-boosts-page "
                                           "#confirmImport').click();"));
  ASSERT_TRUE(
      base::test::RunUntil([&] { return !store()->boosts().empty(); }));
  const Boost added = store()->boosts()[0];
  EXPECT_EQ(added.author, kAuthorShared);
  EXPECT_TRUE(added.enabled);
  EXPECT_EQ(added.name, "Shared colors");
  EXPECT_EQ(added.js_world, kMainWorld);
  EXPECT_EQ(WaitFor(preview, "deepQuery('compass-boosts-page .boosts-badge')"
                             "?.textContent"),
            "Shared");

  // The same link again: another Boost, not a replacement.
  ASSERT_TRUE(content::ExecJs(original, "location.href = '" + link + "';"));
  ASSERT_TRUE(base::test::RunUntil([&] { return tabs()->count() == 3; }));
  content::WebContents* again = tabs()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(again));
  EXPECT_EQ(WaitFor(again, "deepQuery('compass-boosts-page #boostImport')"
                           "?.hidden === false"),
            true);
  ASSERT_TRUE(content::ExecJs(again, std::string(kDeepQuery) +
                                         "deepQuery('compass-boosts-page "
                                         "#confirmImport').click();"));
  ASSERT_TRUE(
      base::test::RunUntil([&] { return store()->boosts().size() == 2u; }));
  EXPECT_NE(store()->boosts()[1].id, added.id);
  EXPECT_EQ(*store()->Find(added.id), added);
}

// A link opened in a new tab (as from another app) shows the preview in
// that tab.
IN_PROC_BROWSER_TEST_F(BoostShareBrowserTest, LinkInANewTab) {
  const int before = tabs()->count();
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL(ShareLink(MakeBoost())),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_NO_WAIT);
  content::WebContents* preview = WaitForPreview(browser());
  ASSERT_TRUE(preview);
  EXPECT_EQ(tabs()->count(), before + 1);
  EXPECT_EQ(tabs()->GetActiveWebContents(), preview);
}

// A broken link says so and adds nothing.
IN_PROC_BROWSER_TEST_F(BoostShareBrowserTest, BrokenLink) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://settings/boosts?import=bm90IGEgYm9vc3Q")));
  content::WebContents* contents = tabs()->GetActiveWebContents();
  EXPECT_EQ(WaitFor(contents,
                    "deepQuery('compass-boosts-page #importNote')?.hidden "
                    "=== false && deepQuery('compass-boosts-page "
                    "#confirmImport').hidden"),
            true);
  EXPECT_TRUE(store()->boosts().empty());
}

// Off the record, the preview opens in a normal window, for the profile's
// Boosts.
IN_PROC_BROWSER_TEST_F(BoostShareBrowserTest, IncognitoOpensANormalWindow) {
  BrowserWindowInterface* incognito = CreateIncognitoBrowser();
  ui_test_utils::NavigateToURLWithDisposition(
      incognito, GURL(ShareLink(MakeBoost())),
      WindowOpenDisposition::CURRENT_TAB, ui_test_utils::BROWSER_TEST_NO_WAIT);
  content::WebContents* preview = WaitForPreview(browser());
  ASSERT_TRUE(preview);
  EXPECT_FALSE(preview->GetBrowserContext()->IsOffTheRecord());
}

// Share gives a link that decodes to the Boost, and copies it.
IN_PROC_BROWSER_TEST_F(BoostShareBrowserTest, ShareCopiesALinkThatDecodes) {
  base::expected<Boost, std::string> saved = store()->Save(MakeBoost());
  ASSERT_TRUE(saved.has_value());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://settings/boosts")));
  content::WebContents* contents = tabs()->GetActiveWebContents();
  const std::string row =
      "deepQuery('compass-boosts-page li[data-id=\"" + saved->id + "\"]";
  ASSERT_EQ(WaitFor(contents, "(b => b ? (b.click(), true) : false)(" + row +
                                  " .boosts-share-button'))"),
            true);
  const std::string text =
      WaitFor(contents, "deepQuery('compass-boosts-page .boosts-share')"
                        "?.dataset.link")
          .ExtractString();
  ASSERT_TRUE(text.starts_with(kShareLinkPrefix));
  std::optional<std::string> fragment = ShareFragmentFromUrl(GURL(text));
  ASSERT_TRUE(fragment);
  base::expected<Boost, ShareError> decoded = DecodeShareFragment(*fragment);
  ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
  EXPECT_EQ(decoded->name, saved->name);
  EXPECT_EQ(decoded->match, saved->match);
  EXPECT_EQ(decoded->css, saved->css);
  EXPECT_EQ(decoded->js, saved->js);
  EXPECT_EQ(decoded->js_world, saved->js_world);

  ASSERT_TRUE(content::ExecJs(contents, std::string(kDeepQuery) +
                                            "deepQuery('compass-boosts-page "
                                            ".boosts-share button').click();"));
  EXPECT_EQ(WaitFor(contents, "deepQuery('compass-boosts-page "
                              ".boosts-share-note')?.textContent"),
            "Link copied.");
  base::test::TestFuture<std::u16string> copied;
  ui::Clipboard::GetForCurrentThread()->ReadText(
      ui::ClipboardBuffer::kCopyPaste, /*data_dst=*/std::nullopt,
      copied.GetCallback());
  EXPECT_EQ(base::UTF16ToUTF8(copied.Get()), text);
}

}  // namespace
}  // namespace compass::boosts
