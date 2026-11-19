// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/base64.h"
#include "base/run_loop.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::easel {
namespace {

class EaselUIBrowserTest : public InProcessBrowserTest {
 protected:
  easels::EaselStore* store() {
    easels::EaselStore* store =
        easels::EaselStore::Get(browser()->GetProfile());
    if (store && !store->loaded()) {
      base::RunLoop loop;
      store->WhenLoaded(loop.QuitClosure());
      loop.Run();
    }
    return store;
  }

  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  // Resolves with |expression|'s value once it's truthy.
  content::EvalJsResult WaitFor(const std::string& expression) {
    return content::EvalJs(contents(), R"(
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
};

// A board with a text object opens in its editor, which shows the text and
// names the tab after the board.
IN_PROC_BROWSER_TEST_F(EaselUIBrowserTest, ShowsABoard) {
  ASSERT_TRUE(store());
  base::expected<sync_pb::CompassEasel, std::string> easel =
      store()->CreateEasel("Trip ideas");
  ASSERT_TRUE(easel.has_value()) << easel.error();
  sync_pb::CompassEaselObject object;
  object.set_easel_uuid(easel->uuid());
  object.set_x(40);
  object.set_y(60);
  object.set_width(240);
  object.set_height(40);
  object.mutable_text()->set_text("Hello easel");
  base::expected<sync_pb::CompassEaselObject, std::string> saved =
      store()->SaveObject(object);
  ASSERT_TRUE(saved.has_value()) << saved.error();

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://easel/" + easel->uuid())));
  EXPECT_EQ("Hello easel",
            WaitFor("document.querySelector('[data-uuid=\"" + saved->uuid() +
                    "\"]')?.textContent"));
  EXPECT_EQ("Trip ideas", WaitFor("document.title"));

  // An edit from elsewhere (another window, sync) shows at once.
  object = *saved;
  object.mutable_text()->set_text("Changed elsewhere");
  ASSERT_TRUE(store()->SaveObject(object).has_value());
  EXPECT_EQ("Changed elsewhere",
            WaitFor("(() => { const t = document.querySelector("
                    "'[data-uuid=\"" + saved->uuid() + "\"]')?.textContent; "
                    "return t === 'Changed elsewhere' && t; })()"));
}

// chrome://easel/ lists the boards.
IN_PROC_BROWSER_TEST_F(EaselUIBrowserTest, ListsBoards) {
  ASSERT_TRUE(store());
  ASSERT_TRUE(store()->CreateEasel("Moodboard").has_value());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://easel/")));
  EXPECT_EQ(true, WaitFor("[...document.querySelectorAll('.easel-title')]"
                          ".some(e => e.textContent === 'Moodboard')"));
}

// The boards' images come from the store.
IN_PROC_BROWSER_TEST_F(EaselUIBrowserTest, ServesAssets) {
  ASSERT_TRUE(store());
  // A 1x1 PNG. The page shows images with <img> (fetch() can't load
  // chrome:// URLs), so that's what's checked.
  std::string png;
  ASSERT_TRUE(base::Base64Decode(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGA"
      "hKmMIQAAAABJRU5ErkJggg==",
      &png));
  base::expected<std::string, std::string> sha256 =
      store()->AddAsset(png, "image/png");
  ASSERT_TRUE(sha256.has_value()) << sha256.error();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://easel/")));
  constexpr char kLoad[] = R"js(
    new Promise(resolve => {
      const image = new Image();
      image.onload = () => resolve(image.naturalWidth);
      image.onerror = () => resolve(-1);
      image.src = '/asset/' + $1;
    }))js";
  EXPECT_EQ(1, content::EvalJs(contents(), content::JsReplace(kLoad, *sha256)));
  // One it doesn't have fails.
  EXPECT_EQ(-1, content::EvalJs(contents(),
                                content::JsReplace(kLoad, std::string(64, '0'))));
}

}  // namespace
}  // namespace compass::easel
