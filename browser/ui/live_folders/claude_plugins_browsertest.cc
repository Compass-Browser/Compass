// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Claude's plugin tools (docs/design/claude.md, "Plugins") with the real
// live folder service: a preview tested on a page, saved with the user's
// approval as a local plugin that then previews the tab, and undone.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_tools.h"
#include "compass/browser/ui/live_folders/claude_plugins.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "compass/browser/ui/live_folders/tab_preview_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"

namespace compass {
namespace {

constexpr char kManifest[] = R"({
  "id": "inbox", "name": "Inbox rows", "version": "1", "api": 1,
  "folder": false,
  "preview": {"matches": ["http://inbox.example.com/*"],
              "select": {"rows": {"selector": "li", "attrs": ["data-id"],
                                  "text": true}}}
})";

constexpr char kMainJs[] = R"(
async function previewTab(page) {
  compass.log('rows: ' + page.groups.rows.length);
  return {title: 'Inbox', items: page.groups.rows.map(
      (r) => ({id: r.attrs['data-id'], title: r.text}))};
}
)";

class PluginToolsBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        [](const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          if (request.relative_url != "/inbox") {
            return nullptr;
          }
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_content_type("text/html");
          response->set_content(
              "<title>Inbox</title><ul><li data-id=1>Alpha</li>"
              "<li data-id=2>Beta</li></ul>");
          return response;
        }));
    ASSERT_TRUE(embedded_test_server()->Start());

    tools_ = std::make_unique<claude::ClaudeTools>();
    tools_->SetFiles(
        base::BindRepeating([] { return claude::FilesConfig(); }),
        base::BindLambdaForTesting(
            [this](claude::ApprovalRequest request,
                   base::OnceCallback<void(bool)> done) {
              requests_.push_back(request);
              std::move(done).Run(allow_);
            }));
    tools_->SetPlugins(
        std::make_unique<ClaudePlugins>(browser()->GetProfile()),
        base::BindLambdaForTesting([this] { return enabled_; }));
  }

  void TearDownOnMainThread() override {
    tools_.reset();
    InProcessBrowserTest::TearDownOnMainThread();
  }

  LiveFolderService* service() {
    return LiveFolderService::Get(browser()->GetProfile());
  }

  GURL InboxUrl() {
    return embedded_test_server()->GetURL("inbox.example.com", "/inbox");
  }

  content::WebContents* OpenInbox() {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(browser(), InboxUrl()));
    content::WebContents* contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    tools_->SetTarget(contents);
    return contents;
  }

  claude::ToolResult Run(const std::string& name, base::DictValue input) {
    base::test::TestFuture<claude::ToolResult> result;
    tools_->Run(name, std::move(input), result.GetCallback());
    return result.Take();
  }

  static std::string Text(const claude::ToolResult& result) {
    const base::Value& first = result.content.front();
    const std::string* text = first.GetDict().FindString("text");
    return text ? *text : std::string();
  }

  static base::DictValue Reply(const claude::ToolResult& result) {
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(Text(result), base::JSON_PARSE_RFC);
    return dict ? std::move(*dict) : base::DictValue();
  }

  std::unique_ptr<claude::ClaudeTools> tools_;
  std::vector<claude::ApprovalRequest> requests_;
  bool allow_ = true;
  bool enabled_ = true;
};

IN_PROC_BROWSER_TEST_F(PluginToolsBrowserTest, TestSaveAndUndo) {
  ASSERT_TRUE(service());
  content::WebContents* contents = OpenInbox();

  // The built-ins are there to learn from.
  claude::ToolResult listed = Run("list_plugins", base::DictValue());
  ASSERT_FALSE(listed.is_error) << Text(listed);
  EXPECT_NE(Text(listed).find("compass.github"), std::string::npos);

  // A test reads the tab, without asking or saving anything.
  claude::ToolResult tested =
      Run("test_plugin", base::DictValue()
                             .Set("manifest", kManifest)
                             .Set("main_js", kMainJs)
                             .Set("mode", "preview"));
  ASSERT_FALSE(tested.is_error) << Text(tested);
  base::DictValue reply = Reply(tested);
  EXPECT_EQ(reply.FindBool("ok"), true) << Text(tested);
  const base::ListValue* items = reply.FindListByDottedPath("result.items");
  ASSERT_TRUE(items);
  ASSERT_EQ(items->size(), 2u);
  EXPECT_EQ(*(*items)[0].GetDict().FindString("title"), "Alpha");
  EXPECT_EQ(*reply.FindList("log"), base::ListValue().Append("rows: 2"));
  EXPECT_NE(reply.FindString("snapshot")->find("Beta"), std::string::npos);
  EXPECT_TRUE(requests_.empty());
  EXPECT_FALSE(service()->registry().Find("claude.inbox"));

  // A preview without the session can't make requests.
  claude::ToolResult fetching = Run(
      "test_plugin",
      base::DictValue()
          .Set("manifest", kManifest)
          .Set("main_js",
               "async function previewTab() {"
               "  await compass.fetch('http://inbox.example.com/x');"
               "  return {items: []}; }")
          .Set("mode", "preview"));
  ASSERT_FALSE(fetching.is_error) << Text(fetching);
  EXPECT_EQ(*Reply(fetching).FindStringByDottedPath("error.code"),
            "host_not_allowed");

  // Saving asks, every time, with the code; the approval turns it on.
  claude::ToolResult saved = Run("save_plugin", base::DictValue()
                                                    .Set("manifest", kManifest)
                                                    .Set("main_js", kMainJs));
  ASSERT_FALSE(saved.is_error) << Text(saved);
  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_TRUE(requests_[0].always_ask);
  EXPECT_NE(requests_[0].detail.find("compass.log('rows: '"),
            std::string::npos);
  EXPECT_NE(requests_[0].detail.find("inbox.example.com"), std::string::npos);
  ASSERT_TRUE(saved.plugin_change);
  EXPECT_EQ(saved.plugin_change->kind, "saved");
  EXPECT_EQ(saved.plugin_change->id, "claude.inbox");
  const LivePlugin* plugin = service()->registry().Find("claude.inbox");
  ASSERT_TRUE(plugin);
  EXPECT_TRUE(plugin->local);
  EXPECT_EQ(service()->previews().FindPlugin(InboxUrl()), plugin);

  // It previews the tab, from its saved files.
  base::test::TestFuture<std::optional<TabPreview>> preview;
  service()->previews().Fetch(contents, InboxUrl(), preview.GetCallback());
  std::optional<TabPreview> shown = preview.Take();
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown->plugin_id, "claude.inbox");
  ASSERT_EQ(shown->result.items.size(), 2u);
  EXPECT_EQ(shown->result.items[1].title, u"Beta");

  // Undone from the chat: gone.
  base::test::TestFuture<bool> undone;
  auto undoer = std::make_unique<ClaudePlugins>(browser()->GetProfile());
  undoer->Undo(*saved.plugin_change, undone.GetCallback());
  EXPECT_TRUE(undone.Get());
  EXPECT_FALSE(service()->registry().Find("claude.inbox"));
}

IN_PROC_BROWSER_TEST_F(PluginToolsBrowserTest, AsksAndRefuses) {
  ASSERT_TRUE(service());
  OpenInbox();

  // Built-ins can't be replaced.
  claude::ToolResult builtin = Run(
      "save_plugin",
      base::DictValue()
          .Set("manifest", R"({"id": "compass.github", "name": "Mine",
                               "version": "1", "api": 1, "hosts": []})")
          .Set("main_js", "async function fetchItems() {}"));
  EXPECT_TRUE(builtin.is_error);
  EXPECT_TRUE(requests_.empty());

  // Denied: nothing is saved.
  allow_ = false;
  claude::ToolResult denied = Run("save_plugin",
                                  base::DictValue()
                                      .Set("manifest", kManifest)
                                      .Set("main_js", kMainJs));
  EXPECT_TRUE(denied.is_error);
  EXPECT_EQ(requests_.size(), 1u);
  EXPECT_FALSE(service()->registry().Find("claude.inbox"));

  // Off: every tool refuses.
  enabled_ = false;
  claude::ToolResult off = Run("list_plugins", base::DictValue());
  EXPECT_TRUE(off.is_error);
  EXPECT_NE(Text(off).find("Let Claude make plugins"), std::string::npos);
}

}  // namespace
}  // namespace compass
