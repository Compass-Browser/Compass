// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/boosts/boost_injector.h"
#include "compass/browser/boosts/boost_store.h"
#include "compass/browser/claude/claude_tools.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::boosts {
namespace {

// Records the body's color when the document is parsed (before any
// stylesheet could load late), and has a global of its own.
constexpr char kPage[] = R"(<!doctype html>
<title>Boosted</title>
<script>
  window.pageSecret = 42;
  document.addEventListener('DOMContentLoaded', () => {
    window.colorAtParse = getComputedStyle(document.body).color;
  });
</script>
<p>Hello</p>)";

constexpr char kColor[] = "rgb(1, 2, 3)";
constexpr char kCss[] = "body { color: rgb(1, 2, 3) !important; }";
// What an isolated or a main-world script sees of the page's globals.
constexpr char kProbeJs[] =
    "document.documentElement.dataset.boostSaw = typeof window.pageSecret;";

class BoostsBrowserTest : public InProcessBrowserTest {
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
    WaitForStore();
  }

  Profile* profile() { return browser()->GetProfile(); }
  BoostStore* store() { return BoostStore::Get(profile()); }
  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  void WaitForStore() {
    base::test::TestFuture<void> loaded;
    store()->WhenLoaded(loaded.GetCallback());
    ASSERT_TRUE(loaded.Wait());
  }

  // Until what the store has is registered.
  void WaitForInjector() {
    BoostInjector* injector = BoostInjector::Get(profile());
    ASSERT_TRUE(injector);
    base::test::TestFuture<void> registered;
    injector->WhenRegistered(registered.GetCallback());
    ASSERT_TRUE(registered.Wait());
  }

  std::string Save(const std::string& match,
                   const std::string& css,
                   const std::string& js = "",
                   const std::string& world = kIsolatedWorld) {
    Boost boost;
    boost.match = match;
    boost.css = css;
    boost.js = js;
    boost.js_world = world;
    base::expected<Boost, std::string> saved = store()->Save(boost);
    EXPECT_TRUE(saved.has_value()) << saved.error();
    WaitForInjector();
    return saved.has_value() ? saved->id : std::string();
  }

  void Open(const std::string& host) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL(host, "/page.html")));
  }

  std::string ColorAtParse() {
    return content::EvalJs(contents(), R"(
        new Promise(resolve => {
          const check = () => window.colorAtParse ?
              resolve(window.colorAtParse) : setTimeout(check, 10);
          check();
        }))")
        .ExtractString();
  }

  // What the probe saw ("undefined", "number"), once it ran.
  std::string ProbeSaw() {
    return content::EvalJs(contents(), R"(
        new Promise(resolve => {
          const check = () => document.documentElement.dataset.boostSaw ?
              resolve(document.documentElement.dataset.boostSaw) :
              setTimeout(check, 10);
          check();
        }))")
        .ExtractString();
  }
};

// The CSS is in before the page is parsed, on the site and www., and not on
// other sites.
IN_PROC_BROWSER_TEST_F(BoostsBrowserTest, CssAppliesAtDocumentStart) {
  Save("a.test", kCss);
  Open("a.test");
  EXPECT_EQ(ColorAtParse(), kColor);
  Open("www.a.test");
  EXPECT_EQ(ColorAtParse(), kColor);
  Open("b.test");
  EXPECT_NE(ColorAtParse(), kColor);
  // Not a subdomain without "*.".
  Open("sub.a.test");
  EXPECT_NE(ColorAtParse(), kColor);
}

IN_PROC_BROWSER_TEST_F(BoostsBrowserTest, SubdomainsAndPaths) {
  Save("*.a.test", kCss);
  Open("sub.a.test");
  EXPECT_EQ(ColorAtParse(), kColor);
  Save("b.test/other", kCss);
  Open("b.test");
  EXPECT_NE(ColorAtParse(), kColor);
}

// Isolated: the page's DOM, not its globals.
IN_PROC_BROWSER_TEST_F(BoostsBrowserTest, IsolatedJsDoesntSeePageGlobals) {
  Save("a.test", "", kProbeJs, kIsolatedWorld);
  Open("a.test");
  EXPECT_EQ(ProbeSaw(), "undefined");
}

IN_PROC_BROWSER_TEST_F(BoostsBrowserTest, MainWorldJsSeesPageGlobals) {
  Save("a.test", "", kProbeJs, kMainWorld);
  Open("a.test");
  EXPECT_EQ(ProbeSaw(), "number");
}

// Turned off, a Boost is gone from the next load; on again, it's back.
// Edited, the new CSS replaces the old.
IN_PROC_BROWSER_TEST_F(BoostsBrowserTest, DisableEditAndDelete) {
  const std::string id = Save("a.test", kCss);
  Open("a.test");
  EXPECT_EQ(ColorAtParse(), kColor);

  ASSERT_TRUE(store()->SetEnabled(id, false));
  WaitForInjector();
  EXPECT_TRUE(BoostInjector::Get(profile())->registered_ids().empty());
  Open("a.test");
  EXPECT_NE(ColorAtParse(), kColor);

  ASSERT_TRUE(store()->SetEnabled(id, true));
  Boost edited = *store()->Find(id);
  edited.css = "body { color: rgb(4, 5, 6) !important; }";
  ASSERT_TRUE(store()->Save(edited).has_value());
  WaitForInjector();
  EXPECT_EQ(BoostInjector::Get(profile())->registered_ids().size(), 1u);
  Open("a.test");
  EXPECT_EQ(ColorAtParse(), "rgb(4, 5, 6)");

  ASSERT_TRUE(store()->Remove(id));
  WaitForInjector();
  Open("a.test");
  EXPECT_NE(ColorAtParse(), "rgb(4, 5, 6)");
}

// Claude's tools: CSS saves at once, JavaScript asks every time, a denied
// change saves nothing, and the setting gates them all.
class BoostToolsBrowserTest : public BoostsBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    BoostsBrowserTest::SetUpOnMainThread();
    tools_ = std::make_unique<claude::ClaudeTools>();
    tools_->SetFiles(
        base::BindRepeating([] { return claude::FilesConfig(); }),
        base::BindLambdaForTesting(
            [this](claude::ApprovalRequest request,
                   base::OnceCallback<void(bool)> done) {
              requests_.push_back(request);
              std::move(done).Run(allow_);
            }));
    tools_->SetBoosts(
        store(), base::BindLambdaForTesting([this] { return enabled_; }),
        base::BindLambdaForTesting([this](base::OnceClosure done) {
          BoostInjector::Get(profile())->WhenRegistered(std::move(done));
        }));
  }

  void TearDownOnMainThread() override {
    tools_.reset();
    BoostsBrowserTest::TearDownOnMainThread();
  }

  claude::ToolResult Run(const std::string& name, const std::string& input) {
    base::test::TestFuture<claude::ToolResult> result;
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(input, base::JSON_PARSE_RFC);
    EXPECT_TRUE(dict) << input;
    tools_->Run(name, std::move(*dict), result.GetCallback());
    return result.Take();
  }

  static std::string Text(const claude::ToolResult& result) {
    const base::Value& first = result.content.front();
    const std::string* text = first.GetDict().FindString("text");
    return text ? *text : std::string();
  }

  std::unique_ptr<claude::ClaudeTools> tools_;
  std::vector<claude::ApprovalRequest> requests_;
  bool allow_ = true;
  bool enabled_ = true;
};

IN_PROC_BROWSER_TEST_F(BoostToolsBrowserTest, SaveBoost) {
  // CSS only: saved at once, Claude's, with an undo for the chat.
  claude::ToolResult saved = Run(
      "save_boost",
      R"({"name": "Colors", "match": "https://www.a.test/", "css": ")" +
          std::string(kCss) + R"("})");
  ASSERT_FALSE(saved.is_error) << Text(saved);
  EXPECT_TRUE(requests_.empty());
  ASSERT_TRUE(saved.boost_change);
  EXPECT_EQ(saved.boost_change->kind, "saved");
  EXPECT_EQ(saved.boost_change->match, "a.test");
  std::optional<base::DictValue> reply =
      base::JSONReader::ReadDict(Text(saved), base::JSON_PARSE_RFC);
  ASSERT_TRUE(reply);
  const std::string id = *reply->FindString("id");
  EXPECT_EQ(reply->FindBool("needs_reload"), true);
  ASSERT_TRUE(store()->Find(id));
  EXPECT_EQ(store()->Find(id)->author, kAuthorClaude);
  // Registered before the result: a reload gets it.
  Open("a.test");
  EXPECT_EQ(ColorAtParse(), kColor);

  // JavaScript: asked, every time, with the code shown.
  claude::ToolResult with_js =
      Run("save_boost", R"({"id": ")" + id +
                            R"x(", "name": "Colors", "match": "a.test",
                            "js": "console.log('boost')"})x");
  ASSERT_FALSE(with_js.is_error) << Text(with_js);
  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_TRUE(requests_[0].always_ask);
  EXPECT_NE(requests_[0].detail.find("console.log('boost')"),
            std::string::npos);
  EXPECT_EQ(with_js.boost_change->kind, "updated");
  ASSERT_TRUE(with_js.boost_change->before);
  EXPECT_TRUE(with_js.boost_change->before->js.empty());
  EXPECT_EQ(store()->Find(id)->js, "console.log('boost')");
  // The CSS stays: fields left out keep their value.
  EXPECT_EQ(store()->Find(id)->css, kCss);

  // CSS again, same JavaScript: no question.
  ASSERT_FALSE(Run("save_boost", R"({"id": ")" + id +
                                     R"(", "name": "Colors", "match": "a.test",
                                     "css": "p {}"})")
                   .is_error);
  EXPECT_EQ(requests_.size(), 1u);

  // Other JavaScript, denied: nothing changes.
  allow_ = false;
  claude::ToolResult denied =
      Run("save_boost", R"({"id": ")" + id +
                            R"x(", "name": "Colors", "match": "a.test",
                            "js": "alert(1)"})x");
  EXPECT_TRUE(denied.is_error);
  EXPECT_EQ(requests_.size(), 2u);
  EXPECT_EQ(store()->Find(id)->js, "console.log('boost')");

  // Deleting a Boost with JavaScript asks too.
  claude::ToolResult kept = Run("delete_boost", R"({"id": ")" + id + R"("})");
  EXPECT_TRUE(kept.is_error);
  EXPECT_EQ(requests_.size(), 3u);
  EXPECT_TRUE(store()->Find(id));
  allow_ = true;
  claude::ToolResult deleted =
      Run("delete_boost", R"({"id": ")" + id + R"("})");
  ASSERT_FALSE(deleted.is_error) << Text(deleted);
  EXPECT_FALSE(store()->Find(id));
  EXPECT_EQ(deleted.boost_change->kind, "deleted");

  // Bad input is refused before anyone is asked.
  EXPECT_TRUE(Run("save_boost", R"({"name": "x", "match": "*", "css": "a{}"})")
                  .is_error);
  EXPECT_TRUE(
      Run("save_boost", R"({"name": "x", "match": "a.test"})").is_error);
  EXPECT_EQ(requests_.size(), 4u);
}

IN_PROC_BROWSER_TEST_F(BoostToolsBrowserTest, ListAndSetting) {
  EXPECT_EQ(Text(Run("list_boosts", "{}")), "No Boosts yet.");
  Save("*.a.test", kCss);
  Save("b.test", "p {}");
  claude::ToolResult listed = Run("list_boosts", R"({"host": "x.a.test"})");
  ASSERT_FALSE(listed.is_error);
  std::optional<base::Value> list =
      base::JSONReader::Read(Text(listed), base::JSON_PARSE_RFC);
  ASSERT_TRUE(list && list->is_list());
  ASSERT_EQ(list->GetList().size(), 1u);
  EXPECT_EQ(*list->GetList()[0].GetDict().FindString("css"), kCss);

  // Off: every Boost tool refuses.
  enabled_ = false;
  claude::ToolResult off = Run("list_boosts", "{}");
  EXPECT_TRUE(off.is_error);
  EXPECT_NE(Text(off).find("Let Claude make Boosts"), std::string::npos);
  EXPECT_TRUE(Run("save_boost", R"({"name": "x", "match": "c.test",
                                    "css": "a{}"})")
                  .is_error);
  EXPECT_EQ(store()->boosts().size(), 2u);

  // Sent only when asked for.
  const auto has = [](const base::ListValue& tools, const char* name) {
    for (const base::Value& tool : tools) {
      const std::string* tool_name = tool.GetDict().FindString("name");
      if (tool_name && *tool_name == name) {
        return true;
      }
    }
    return false;
  };
  EXPECT_FALSE(has(claude::ClaudeTools::Definitions(), "save_boost"));
  EXPECT_TRUE(has(claude::ClaudeTools::Definitions(
                      /*memory=*/false, /*files=*/true, /*devtools=*/false,
                      /*boosts=*/true),
                  "save_boost"));
}

}  // namespace
}  // namespace compass::boosts
