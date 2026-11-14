// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/today_tidier.h"

#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
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
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace compass {
namespace {

using NameSource = SidebarNode::NameSource;

constexpr char kKey[] = "sk-ant-api03-tidy-today-test-key";

// Claude's reply: two folders of Today's first five loose entries.
constexpr char kTidyReply[] =
    R"({"folders": [{"name": "Fox Facts", "entries": ["1", "2"]},)"
    R"( {"name": "Fox News", "entries": ["3", "4", "5"]}]})";

// The fake Claude API's stream for a reply of |text|.
std::string Stream(const std::string& text) {
  auto event = [](const char* name, base::DictValue data) {
    return base::StrCat(
        {"event: ", name, "\ndata: ", *base::WriteJson(data), "\n\n"});
  };
  return base::StrCat(
      {event("message_start",
             base::DictValue().Set("type", "message_start")
                 .Set("message", base::DictValue()
                                     .Set("id", "msg_1")
                                     .Set("type", "message")
                                     .Set("role", "assistant")
                                     .Set("content", base::ListValue()))),
       event("content_block_start",
             base::DictValue()
                 .Set("type", "content_block_start")
                 .Set("index", 0)
                 .Set("content_block",
                      base::DictValue().Set("type", "text").Set("text", ""))),
       event("content_block_delta",
             base::DictValue()
                 .Set("type", "content_block_delta")
                 .Set("index", 0)
                 .Set("delta", base::DictValue()
                                   .Set("type", "text_delta")
                                   .Set("text", text))),
       event("content_block_stop",
             base::DictValue().Set("type", "content_block_stop").Set("index",
                                                                     0)),
       event("message_delta",
             base::DictValue()
                 .Set("type", "message_delta")
                 .Set("delta",
                      base::DictValue().Set("stop_reason", "end_turn"))),
       event("message_stop", base::DictValue().Set("type", "message_stop"))});
}

// Waits until the sidebar page shows Tidy in Today's header ($1: true), or
// shows Today's header without it (false); true once it does.
constexpr char kWaitForTidyButton[] = R"(
  (async (shown) => {
    const root = () =>
        document.querySelector('compass-sidebar-app')?.shadowRoot;
    for (let i = 0; i < 500; ++i) {
      const button = root()?.querySelector('#tidyToday');
      if (shown ? (button && !button.disabled) :
                  (root()?.querySelector('#today') && !button)) {
        return true;
      }
      await new Promise(r => setTimeout(r, 20));
    }
    return false;
  })($1))";

// Waits for the toast, returns its text and clicks its Undo.
constexpr char kUndoToast[] = R"(
  (async () => {
    const root = () =>
        document.querySelector('compass-sidebar-app')?.shadowRoot;
    for (let i = 0; i < 500; ++i) {
      const undo = root()?.querySelector('#toastUndo');
      if (undo) {
        const text = root().querySelector('#toast .text').textContent;
        undo.click();
        return text;
      }
      await new Promise(r => setTimeout(r, 20));
    }
    return '';
  })())";

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

class TodayTidierBrowserTest : public InProcessBrowserTest,
                               public TodayTidier::Observer {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &TodayTidierBrowserTest::Serve, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());

    Profile* profile = browser()->GetProfile();
    PrefService* prefs = profile->GetPrefs();
    prefs->SetString(claude::prefs::kBaseUrl,
                     embedded_test_server()->base_url().spec());
    base::test::TestFuture<bool> saved;
    claude::ClaudeCredentials::SetApiKey(profile, kKey, saved.GetCallback());
    ASSERT_TRUE(saved.Get());

    tidier_ = TodayTidier::Get(profile);
    ASSERT_TRUE(tidier_);
    tidier_->AddObserver(this);
  }

  void TearDownOnMainThread() override {
    tidier_->RemoveObserver(this);
    tidier_ = nullptr;
    InProcessBrowserTest::TearDownOnMainThread();
  }

  // TodayTidier::Observer:
  void OnTidied(const base::Uuid& space_id,
                int undo_id,
                size_t folders) override {
    undo_id_ = undo_id;
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
      response->set_content(Stream(kTidyReply));
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

  // Opens /fox/<name> in a new background tab, typed: a loose Today entry.
  void OpenTab(const std::string& name) {
    NavigateParams params(browser(),
                          embedded_test_server()->GetURL("/fox/" + name),
                          ui::PAGE_TRANSITION_TYPED);
    params.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    Navigate(&params);
    ASSERT_TRUE(params.navigated_or_inserted_contents);
    EXPECT_TRUE(
        content::WaitForLoadStop(params.navigated_or_inserted_contents));
  }

  // At least |count| loose entries in Today.
  void OpenLooseEntries(size_t count) {
    for (int i = 0; TodayTidier::LooseEntries(space_model()).size() < count;
         ++i) {
      ASSERT_LT(i, 20);
      OpenTab(base::NumberToString(i));
    }
    // Their titles come a moment after their pages.
    ASSERT_TRUE(base::test::RunUntil([&] {
      for (const SidebarNode* entry :
           TodayTidier::LooseEntries(space_model())) {
        if (entry->type() == SidebarNode::Type::kTab &&
            entry->url().path().starts_with("/fox/") &&
            !entry->title().starts_with(u"Fox ")) {
          return false;
        }
      }
      return true;
    }));
  }

  // Today's top-level entries, by ID.
  std::vector<base::Uuid> TodayIds() {
    std::vector<base::Uuid> ids;
    for (const auto& entry : space_model().today_root()->children()) {
      ids.push_back(entry->id());
    }
    return ids;
  }

  size_t TodayFolders() {
    size_t folders = 0;
    for (const auto& entry : space_model().today_root()->children()) {
      folders += entry->type() == SidebarNode::Type::kFolder;
    }
    return folders;
  }

  content::WebContents* sidebar_contents() {
    auto* sidebar = FindDescendant<CompassSidebarView>(
        BrowserView::GetBrowserViewForBrowser(browser()));
    EXPECT_TRUE(sidebar);
    if (!sidebar) {
      return nullptr;
    }
    content::WebContents* contents = sidebar->GetWebContents();
    EXPECT_TRUE(content::WaitForLoadStop(contents));
    return contents;
  }

  // Runs the loop for |delay|, for checks that nothing happens.
  static void Wait(base::TimeDelta delay) {
    base::RunLoop loop;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), delay);
    loop.Run();
  }

  base::Lock lock_;
  std::vector<std::string> bodies_;
  raw_ptr<TodayTidier> tidier_ = nullptr;
  int undo_id_ = 0;
};

// With "Tidy Today" on, Tidy groups Today's loose entries into Claude's
// folders, from their titles and hosts only; Undo puts them back.
IN_PROC_BROWSER_TEST_F(TodayTidierBrowserTest, TidiesAndUndoes) {
  browser()->GetProfile()->GetPrefs()->SetBoolean(claude::prefs::kTidyToday,
                                                  true);
  OpenLooseEntries(6);
  const std::vector<base::Uuid> before = TodayIds();
  const std::vector<const SidebarNode*> loose =
      TodayTidier::LooseEntries(space_model());
  ASSERT_TRUE(tidier_->CanTidy(space_model()));
  const int tabs = browser()->tab_strip_model()->count();

  tidier_->Tidy(space_model());
  // Waiting for Claude; once at a time.
  EXPECT_TRUE(tidier_->IsBusy(space_model().id()));
  EXPECT_FALSE(tidier_->CanTidy(space_model()));
  ASSERT_TRUE(base::test::RunUntil([&] { return TodayFolders() == 2; }));
  EXPECT_FALSE(tidier_->IsBusy(space_model().id()));
  EXPECT_EQ(Requests(), 1u);

  const SidebarNode* facts = space_model().today_root()->children()[0].get();
  EXPECT_EQ(facts->title(), u"Fox Facts");
  EXPECT_EQ(facts->name_source(), NameSource::kClaude);
  EXPECT_EQ(facts->children().size(), 2u);
  const SidebarNode* news = space_model().today_root()->children()[1].get();
  EXPECT_EQ(news->title(), u"Fox News");
  EXPECT_EQ(news->children().size(), 3u);
  // The rest stay loose.
  EXPECT_EQ(TodayTidier::LooseEntries(space_model()).size(), loose.size() - 5);

  const std::string body = LastBody();
  EXPECT_NE(body.find("Fox 0"), std::string::npos);
  EXPECT_NE(body.find("127.0.0.1"), std::string::npos);
  // Hosts, never paths.
  EXPECT_EQ(body.find("/fox/"), std::string::npos);
  EXPECT_NE(body.find("claude-haiku-5-5"), std::string::npos);
  EXPECT_EQ(body.find("\"tools\""), std::string::npos);
  // No tab closed.
  EXPECT_EQ(browser()->tab_strip_model()->count(), tabs);

  ASSERT_NE(undo_id_, 0);
  EXPECT_TRUE(tidier_->Undo(undo_id_));
  EXPECT_EQ(TodayIds(), before);
  EXPECT_EQ(TodayFolders(), 0u);
}

// The same through the sidebar page: Tidy in Today's header, then Undo in
// its toast.
IN_PROC_BROWSER_TEST_F(TodayTidierBrowserTest, TidyButtonAndUndoToast) {
  browser()->GetProfile()->GetPrefs()->SetBoolean(claude::prefs::kTidyToday,
                                                  true);
  OpenLooseEntries(5);
  const std::vector<base::Uuid> before = TodayIds();
  content::WebContents* sidebar = sidebar_contents();
  ASSERT_TRUE(sidebar);
  ASSERT_EQ(true, content::EvalJs(sidebar, content::JsReplace(
                                               kWaitForTidyButton, true)));
  ASSERT_TRUE(content::ExecJs(sidebar, R"(
      document.querySelector('compass-sidebar-app').shadowRoot
          .querySelector('#tidyToday').click();)"));
  ASSERT_TRUE(base::test::RunUntil([&] { return TodayFolders() == 2; }));
  EXPECT_EQ(content::EvalJs(sidebar, kUndoToast), "Tidied into 2 folders");
  ASSERT_TRUE(base::test::RunUntil([&] { return TodayIds() == before; }));
  EXPECT_EQ(TodayFolders(), 0u);
}

// "Tidy Today" is off by default: no Tidy in Today's header or the command
// bar, and no request, however many entries Today has.
IN_PROC_BROWSER_TEST_F(TodayTidierBrowserTest, NothingWithTheSwitchOff) {
  EXPECT_FALSE(browser()->GetProfile()->GetPrefs()->GetBoolean(
      claude::prefs::kTidyToday));
  OpenLooseEntries(6);
  EXPECT_FALSE(tidier_->IsEnabled());
  EXPECT_FALSE(tidier_->CanTidy(space_model()));
  content::WebContents* sidebar = sidebar_contents();
  ASSERT_TRUE(sidebar);
  EXPECT_EQ(true, content::EvalJs(sidebar, content::JsReplace(
                                               kWaitForTidyButton, false)));

  const std::vector<base::Uuid> before = TodayIds();
  tidier_->Tidy(space_model());
  EXPECT_FALSE(tidier_->IsBusy(space_model().id()));
  Wait(base::Milliseconds(500));
  EXPECT_EQ(Requests(), 0u);
  EXPECT_EQ(TodayIds(), before);

  // On, the button comes; off again, it goes.
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  prefs->SetBoolean(claude::prefs::kTidyToday, true);
  EXPECT_EQ(true, content::EvalJs(sidebar, content::JsReplace(
                                               kWaitForTidyButton, true)));
  prefs->SetBoolean(claude::prefs::kTidyToday, false);
  EXPECT_EQ(true, content::EvalJs(sidebar, content::JsReplace(
                                               kWaitForTidyButton, false)));
  EXPECT_EQ(Requests(), 0u);
}

}  // namespace
}  // namespace compass
