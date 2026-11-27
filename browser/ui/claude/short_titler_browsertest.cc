// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/short_titler.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_bubble_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
#include "compass/browser/ui/views/sidebar/sidebar_hover_card.h"
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
#include "ui/views/controls/label.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace compass {
namespace {

constexpr char kKey[] = "sk-ant-api03-short-titles-test-key";

constexpr char kReply[] = R"({"titles": {"1": "Fox facts"}})";

// A long title, with a site's name after it.
constexpr char16_t kLongTitle[] =
    u"Everything you wanted to know about foxes \u00B7 Fox Encyclopedia";

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

// The text of tab row $1's title, and its tooltip, as "text|tooltip", once
// its text is $2 (or after a few seconds).
constexpr char kRowTitle[] = R"(
  (async (id, want) => {
    const root = () =>
        document.querySelector('compass-sidebar-app')?.shadowRoot;
    let row = null;
    for (let i = 0; i < 250; ++i) {
      row = root()?.querySelector('.tab[data-node-id="' + id + '"]');
      if (row && row.querySelector('.title')?.textContent === want) {
        break;
      }
      await new Promise(r => setTimeout(r, 20));
    }
    if (!row) {
      return '';
    }
    return row.querySelector('.title').textContent + '|' + row.title;
  })($1, $2))";

// Rests the pointer on tab row $1.
constexpr char kHover[] = R"(
  ((id) => {
    const root = document.querySelector('compass-sidebar-app').shadowRoot;
    root.querySelector('.tab[data-node-id="' + id + '"]').dispatchEvent(
        new PointerEvent('pointerover', {
          pointerType: 'mouse', bubbles: true, composed: true,
        }));
  })($1))";

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

class ShortTitlerBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &ShortTitlerBrowserTest::Serve, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());

    Profile* profile = browser()->GetProfile();
    PrefService* prefs = profile->GetPrefs();
    prefs->SetString(claude::prefs::kBaseUrl,
                     embedded_test_server()->base_url().spec());
    base::test::TestFuture<bool> saved;
    claude::ClaudeCredentials::SetApiKey(profile, kKey, saved.GetCallback());
    ASSERT_TRUE(saved.Get());

    titler_ = ShortTitler::Get(profile);
    ASSERT_TRUE(titler_);
    titler_->SetBatchDelayForTesting(base::Milliseconds(100));
    ASSERT_TRUE(
        base::test::RunUntil([&] { return titler_->IsObservingForTesting(); }));
  }

  void TearDownOnMainThread() override {
    titler_ = nullptr;
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
      response->set_content(Stream(kReply));
      return response;
    }
    if (request.relative_url.starts_with("/fox")) {
      response->set_content_type("text/html; charset=utf-8");
      response->set_content("<title>" + base::UTF16ToUTF8(kLongTitle) +
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

  // Opens /fox in a new background tab; its Today entry, once it has its
  // page's title.
  const SidebarNode* OpenFox() {
    NavigateParams params(browser(), embedded_test_server()->GetURL("/fox/1"),
                          ui::PAGE_TRANSITION_TYPED);
    params.disposition = WindowOpenDisposition::kNewBackgroundTab;
    Navigate(&params);
    EXPECT_TRUE(params.navigated_or_inserted_contents);
    EXPECT_TRUE(
        content::WaitForLoadStop(params.navigated_or_inserted_contents));
    const SidebarNode* fox = nullptr;
    EXPECT_TRUE(base::test::RunUntil([&] {
      for (const auto& entry : space_model().today_root()->children()) {
        if (entry->type() == SidebarNode::Type::kTab &&
            entry->title() == kLongTitle && !seen_.contains(entry->id())) {
          fox = entry.get();
          seen_.insert(fox->id());
          return true;
        }
      }
      return false;
    }));
    return fox;
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

  static void Wait(base::TimeDelta delay) {
    base::RunLoop loop;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), delay);
    loop.Run();
  }

  base::Lock lock_;
  std::vector<std::string> bodies_;
  raw_ptr<ShortTitler> titler_ = nullptr;
  // The entries OpenFox() gave.
  std::set<base::Uuid> seen_;
};

// With "Shorter Tab Titles" on, a long title gets Claude's short one in the
// sidebar's row; the tooltip, the hover card and the window keep the page's
// title. Asked with the title and host only.
IN_PROC_BROWSER_TEST_F(ShortTitlerBrowserTest, ShowsShortTitles) {
  browser()->GetProfile()->GetPrefs()->SetBoolean(
      claude::prefs::kShortTabTitles, true);
  const SidebarNode* fox = OpenFox();
  ASSERT_TRUE(fox);
  const base::Uuid fox_id = fox->id();
  ASSERT_TRUE(base::test::RunUntil([&] {
    const SidebarNode* node = space_model().FindById(fox_id);
    return node && node->short_title() == u"Fox facts";
  }));
  EXPECT_EQ(Requests(), 1u);
  const std::string body = LastBody();
  EXPECT_NE(body.find("Everything you wanted to know about foxes"),
            std::string::npos);
  EXPECT_NE(body.find("127.0.0.1"), std::string::npos);
  EXPECT_EQ(body.find("/fox/"), std::string::npos);
  EXPECT_NE(body.find("claude-haiku-5-5"), std::string::npos);
  EXPECT_EQ(body.find("\"tools\""), std::string::npos);
  // Not the title anything else reads.
  EXPECT_EQ(space_model().FindById(fox_id)->display_title(), kLongTitle);

  content::WebContents* sidebar = sidebar_contents();
  ASSERT_TRUE(sidebar);
  const std::string id = fox_id.AsLowercaseString();
  EXPECT_EQ(content::EvalJs(sidebar,
                            content::JsReplace(kRowTitle, id, "Fox facts")),
            "Fox facts|" + base::UTF16ToUTF8(kLongTitle));

  // The hover card shows the page's title.
  SidebarHoverCard* card = SidebarHoverCard::FromWebContents(sidebar);
  ASSERT_TRUE(card);
  ASSERT_TRUE(content::ExecJs(sidebar, content::JsReplace(kHover, id)));
  ASSERT_TRUE(base::test::RunUntil([&] { return card->IsShowing(); }));
  EXPECT_EQ(card->bubble_for_testing()->GetTitleViewForTesting()->GetText(),
            kLongTitle);

  // The same title again costs nothing.
  const SidebarNode* again = OpenFox();
  ASSERT_TRUE(again);
  EXPECT_EQ(again->short_title(), u"Fox facts");
  Wait(base::Milliseconds(300));
  EXPECT_EQ(Requests(), 1u);

  // A name the user gives wins.
  SidebarNode* named = space_model().FindById(fox_id);
  space_model().SetCustomTitle(named, u"Mine");
  EXPECT_EQ(content::EvalJs(sidebar, content::JsReplace(kRowTitle, id, "Mine")),
            "Mine|Mine");
}

// Off by default: no request, and rows show the page's title.
IN_PROC_BROWSER_TEST_F(ShortTitlerBrowserTest, NothingWithTheSwitchOff) {
  EXPECT_FALSE(browser()->GetProfile()->GetPrefs()->GetBoolean(
      claude::prefs::kShortTabTitles));
  const SidebarNode* fox = OpenFox();
  ASSERT_TRUE(fox);
  const base::Uuid fox_id = fox->id();
  Wait(base::Milliseconds(500));
  EXPECT_EQ(Requests(), 0u);
  EXPECT_TRUE(space_model().FindById(fox_id)->short_title().empty());
  content::WebContents* sidebar = sidebar_contents();
  ASSERT_TRUE(sidebar);
  const std::string title = base::UTF16ToUTF8(kLongTitle);
  EXPECT_EQ(content::EvalJs(sidebar,
                            content::JsReplace(kRowTitle,
                                               fox_id.AsLowercaseString(),
                                               title)),
            title + "|" + title);
}

}  // namespace
}  // namespace compass
