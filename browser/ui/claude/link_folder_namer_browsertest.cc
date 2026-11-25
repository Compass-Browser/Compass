// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/link_folder_namer.h"

#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"

namespace compass {
namespace {

using NameSource = SidebarNode::NameSource;

constexpr char kKey[] = "sk-ant-api03-link-folder-test-key";

// The fake Claude API's reply: {"name": "Fox Facts"}.
constexpr char kReply[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
    "\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,"
    "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\","
    "\"text\":\"{\\\"name\\\": \\\"Fox Facts\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
    "\"end_turn\"}}\n\n"
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

class LinkFolderNamerBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &LinkFolderNamerBrowserTest::Serve, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());

    Profile* profile = browser()->GetProfile();
    PrefService* prefs = profile->GetPrefs();
    prefs->SetString(claude::prefs::kBaseUrl,
                     embedded_test_server()->base_url().spec());
    base::test::TestFuture<bool> saved;
    claude::ClaudeCredentials::SetApiKey(profile, kKey, saved.GetCallback());
    ASSERT_TRUE(saved.Get());

    namer_ = LinkFolderNamer::Get(profile);
    ASSERT_TRUE(namer_);
    // Long enough for a new tab's title to come in first (up to 200 ms).
    namer_->SetDebounceForTesting(base::Seconds(1));
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return namer_->IsObservingForTesting(); }));
  }

  void TearDownOnMainThread() override {
    namer_ = nullptr;
    InProcessBrowserTest::TearDownOnMainThread();
  }

  std::unique_ptr<net::test_server::HttpResponse> Serve(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    if (request.relative_url == "/v1/messages") {
      {
        base::AutoLock lock(lock_);
        bodies_.push_back(request.content);
      }
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/event-stream");
      response->set_content(kReply);
      return response;
    }
    if (request.relative_url.starts_with("/fox/")) {
      response->set_content_type("text/html");
      response->set_content("<title>Fox " + request.relative_url.substr(5) +
                            "</title><p>Foxes.</p>");
      return response;
    }
    return nullptr;
  }

  size_t Requests() {
    base::AutoLock lock(lock_);
    return bodies_.size();
  }

  std::string LastBody() {
    base::AutoLock lock(lock_);
    return bodies_.empty() ? std::string() : bodies_.back();
  }

  SpaceModel& space_model() {
    return WindowFeatures::From(browser())->space_model();
  }

  // Opens /fox/<name> as a link from the active tab, in the background, as
  // Cmd+click does.
  void OpenLinkInBackground(const std::string& name) {
    NavigateParams params(browser(),
                          embedded_test_server()->GetURL("/fox/" + name),
                          ui::PAGE_TRANSITION_LINK);
    params.source_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    params.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    Navigate(&params);
    ASSERT_TRUE(params.navigated_or_inserted_contents);
    EXPECT_TRUE(
        content::WaitForLoadStop(params.navigated_or_inserted_contents));
  }

  // The link folder at the top of Today, or null.
  SidebarNode* LinkFolder() {
    for (const auto& entry : space_model().today_root()->children()) {
      if (entry->type() == SidebarNode::Type::kFolder) {
        return entry.get();
      }
    }
    return nullptr;
  }

  // Runs the loop for |delay|, for checks that nothing happens.
  static void Wait(base::TimeDelta delay) {
    base::RunLoop loop;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), delay);
    loop.Run();
  }

  void OpenOpener() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("/fox/opener")));
  }

  base::Lock lock_;
  std::vector<std::string> bodies_;
  raw_ptr<LinkFolderNamer> namer_ = nullptr;
};

// With "Name Link Folders" on, a link folder reaching 3 entries is named by Claude,
// from its tabs' titles and hosts only.
IN_PROC_BROWSER_TEST_F(LinkFolderNamerBrowserTest, NamesALinkFolderAtThree) {
  browser()->GetProfile()->GetPrefs()->SetBoolean(
      claude::prefs::kNameLinkFolders, true);
  OpenOpener();
  OpenLinkInBackground("one");
  SidebarNode* folder = LinkFolder();
  ASSERT_TRUE(folder);
  EXPECT_EQ(folder->name_source(), NameSource::kOpener);
  const std::u16string opener_name = folder->title();
  // Two entries: not yet.
  Wait(base::Milliseconds(1500));
  EXPECT_EQ(Requests(), 0u);

  OpenLinkInBackground("two");
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return folder->title() == u"Fox Facts"; }));
  EXPECT_EQ(folder->name_source(), NameSource::kClaude);
  EXPECT_EQ(folder->name_before_claude(), opener_name);
  EXPECT_EQ(Requests(), 1u);
  const std::string body = LastBody();
  EXPECT_NE(body.find("Fox two"), std::string::npos);
  EXPECT_NE(body.find("127.0.0.1"), std::string::npos);
  // Hosts, never paths.
  EXPECT_EQ(body.find("/fox/two"), std::string::npos);
  EXPECT_NE(body.find("claude-haiku-5-5"), std::string::npos);
  EXPECT_EQ(body.find("\"tools\""), std::string::npos);

  // Undo Name: the opener's name, the user's now; more links don't ask.
  EXPECT_TRUE(space_model().UndoFolderName(folder));
  EXPECT_EQ(folder->title(), opener_name);
  for (const char* name : {"three", "four", "five", "six"}) {
    OpenLinkInBackground(name);
  }
  Wait(base::Milliseconds(1500));
  EXPECT_EQ(Requests(), 1u);
  EXPECT_EQ(folder->title(), opener_name);
}

// A folder the user renamed is theirs: Claude doesn't name it.
IN_PROC_BROWSER_TEST_F(LinkFolderNamerBrowserTest, NotAfterAManualRename) {
  browser()->GetProfile()->GetPrefs()->SetBoolean(
      claude::prefs::kNameLinkFolders, true);
  OpenOpener();
  OpenLinkInBackground("one");
  SidebarNode* folder = LinkFolder();
  ASSERT_TRUE(folder);
  space_model().SetFolderName(folder, u"My Foxes", NameSource::kUser);
  OpenLinkInBackground("two");
  OpenLinkInBackground("three");
  Wait(base::Milliseconds(1500));
  EXPECT_EQ(Requests(), 0u);
  EXPECT_EQ(folder->title(), u"My Foxes");
}

// "Name Link Folders" is off by default: no request, whatever the folders
// do.
IN_PROC_BROWSER_TEST_F(LinkFolderNamerBrowserTest, NoRequestWithTheSwitchOff) {
  EXPECT_FALSE(browser()->GetProfile()->GetPrefs()->GetBoolean(
      claude::prefs::kNameLinkFolders));
  OpenOpener();
  for (const char* name : {"one", "two", "three"}) {
    OpenLinkInBackground(name);
  }
  SidebarNode* folder = LinkFolder();
  ASSERT_TRUE(folder);
  EXPECT_EQ(folder->children().size(), 4u);
  Wait(base::Milliseconds(1500));
  EXPECT_EQ(Requests(), 0u);
  EXPECT_EQ(folder->name_source(), NameSource::kOpener);
}

}  // namespace
}  // namespace compass
