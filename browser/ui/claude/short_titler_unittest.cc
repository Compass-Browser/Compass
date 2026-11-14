// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/short_titler.h"

#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_quick.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "components/prefs/testing_pref_service.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

constexpr char kKey[] = "sk-ant-api03-short-titles-test-key";
constexpr char kMessagesUrl[] = "https://api.anthropic.com/v1/messages";

// The fake API's stream for a reply whose text is |json| (no line breaks or
// backslashes in it).
std::string Reply(std::string_view json) {
  std::string escaped;
  for (char c : json) {
    if (c == '"') {
      escaped += '\\';
    }
    escaped += c;
  }
  return base::StrCat(
      {"event: message_start\n"
       "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
       "\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n"
       "event: content_block_start\n"
       "data: {\"type\":\"content_block_start\",\"index\":0,"
       "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
       "event: content_block_delta\n"
       "data: {\"type\":\"content_block_delta\",\"index\":0,"
       "\"delta\":{\"type\":\"text_delta\",\"text\":\"",
       escaped,
       "\"}}\n\n"
       "event: content_block_stop\n"
       "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
       "event: message_delta\n"
       "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
       "\"end_turn\"}}\n\n"
       "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n"});
}

constexpr char16_t kPrTitle[] =
    u"Fix the login redirect loop by kalle \u00B7 Pull Request #123 \u00B7 "
    u"org/repo";
constexpr char16_t kVideoTitle[] = u"Cats - YouTube";

class ShortTitlerTest : public testing::Test {
 protected:
  ShortTitlerTest() {
    claude::prefs::RegisterProfilePrefs(prefs_.registry());
    prefs_.SetString(claude::prefs::kApiKey, "encrypted");
    factory_.SetInterceptor(base::BindRepeating(
        [](std::vector<network::ResourceRequest>* requests,
           const network::ResourceRequest& request) {
          requests->push_back(request);
        },
        base::Unretained(&requests_)));
    quick_ = std::make_unique<claude::ClaudeQuick>(
        &prefs_,
        base::BindRepeating(
            [](network::TestURLLoaderFactory* factory)
                -> scoped_refptr<network::SharedURLLoaderFactory> {
              return base::MakeRefCounted<
                  network::WeakWrapperSharedURLLoaderFactory>(factory);
            },
            base::Unretained(&factory_)),
        base::BindRepeating([](claude::ClaudeQuick::KeyCallback callback) {
          std::move(callback).Run(std::string(kKey));
        }));
    titler_ = std::make_unique<ShortTitler>(
        &prefs_, quick_.get(),
        base::BindRepeating([] { return std::string("en-US"); }));
    titler_->Observe(space_);
  }

  ~ShortTitlerTest() override {
    titler_->Shutdown();
    titler_.reset();
  }

  SidebarNode* AddTab(const std::string& url, std::u16string title) {
    SidebarNode* today = space_.today_root();
    return space_.AddTab(today, today->children().size(), GURL(url),
                         std::move(title));
  }

  void Serve(std::string_view json) {
    factory_.AddResponse(kMessagesUrl, Reply(json));
  }

  // The user message of request |index|.
  std::string UserMessage(size_t index) {
    EXPECT_LT(index, requests_.size());
    if (index >= requests_.size()) {
      return std::string();
    }
    std::optional<base::DictValue> body = base::JSONReader::ReadDict(
        network::GetUploadData(requests_[index]), base::JSON_PARSE_RFC);
    const base::ListValue* messages = body ? body->FindList("messages") : nullptr;
    if (!messages || messages->empty()) {
      return std::string();
    }
    const base::ListValue* content =
        (*messages)[0].GetDict().FindList("content");
    const std::string* text =
        content && !content->empty()
            ? (*content)[0].GetDict().FindString("text")
            : nullptr;
    return text ? *text : std::string();
  }

  base::DictValue Body(size_t index) {
    return base::JSONReader::ReadDict(network::GetUploadData(requests_[index]),
                                      base::JSON_PARSE_RFC)
        .value_or(base::DictValue());
  }

  void TurnOn() { prefs_.SetBoolean(claude::prefs::kShortTabTitles, true); }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  TestingPrefServiceSimple prefs_;
  network::TestURLLoaderFactory factory_;
  std::vector<network::ResourceRequest> requests_;
  std::unique_ptr<claude::ClaudeQuick> quick_;
  SpaceModel space_;
  std::unique_ptr<ShortTitler> titler_;
};

// Off by default: nothing is asked, however long the titles.
TEST_F(ShortTitlerTest, NothingWhileOff) {
  EXPECT_FALSE(titler_->IsEnabled());
  AddTab("https://github.com/org/repo/pull/123", kPrTitle);
  task_environment_.FastForwardBy(base::Minutes(1));
  EXPECT_EQ(titler_->pending_for_testing(), 0u);
  EXPECT_TRUE(requests_.empty());

  // Not set up: no more.
  prefs_.ClearPref(claude::prefs::kApiKey);
  TurnOn();
  EXPECT_FALSE(titler_->IsEnabled());
  task_environment_.FastForwardBy(base::Minutes(1));
  EXPECT_TRUE(requests_.empty());
}

// Titles that changed in the last 10 s go in one request, with their hosts
// (never paths); short ones, and tabs the user named, don't.
TEST_F(ShortTitlerTest, BatchesTitles) {
  TurnOn();
  Serve(R"({"titles": {"1": "PR #123: fix login", "2": ""}})");
  SidebarNode* pr = AddTab("https://github.com/org/repo/pull/123", kPrTitle);
  task_environment_.FastForwardBy(base::Seconds(4));
  SidebarNode* video = AddTab("https://www.youtube.com/watch?v=1", kVideoTitle);
  SidebarNode* plain = AddTab("https://example.com/", u"Example");
  SidebarNode* named = AddTab("https://news.test/a", kPrTitle);
  space_.SetCustomTitle(named, u"Mine");
  EXPECT_EQ(titler_->pending_for_testing(), 2u);

  // 10 s after the first change, not before.
  task_environment_.FastForwardBy(base::Seconds(5));
  EXPECT_TRUE(requests_.empty());
  task_environment_.FastForwardBy(base::Seconds(2));
  ASSERT_EQ(requests_.size(), 1u);
  const std::string user = UserMessage(0);
  EXPECT_NE(user.find("1. " + base::UTF16ToUTF8(kPrTitle) + " (github.com)"),
            std::string::npos);
  EXPECT_NE(user.find("2. Cats - YouTube (www.youtube.com)"),
            std::string::npos);
  EXPECT_EQ(user.find("/pull/"), std::string::npos);
  EXPECT_EQ(user.find("Example"), std::string::npos);
  EXPECT_EQ(user.find("news.test"), std::string::npos);
  base::DictValue body = Body(0);
  EXPECT_EQ(*body.FindString("model"), "claude-haiku-5-5");
  EXPECT_FALSE(body.Find("tools"));
  EXPECT_EQ(quick_->RecentBackgroundRequests(), 1u);

  task_environment_.RunUntilIdle();
  EXPECT_EQ(pr->short_title(), u"PR #123: fix login");
  // Claude gave none; the title stays.
  EXPECT_TRUE(video->short_title().empty());
  EXPECT_TRUE(plain->short_title().empty());
  // The page's title is the title still.
  EXPECT_EQ(pr->title(), kPrTitle);
  EXPECT_EQ(pr->display_title(), kPrTitle);
  // A user's name wins: no short title asked for, or shown.
  EXPECT_TRUE(named->short_title().empty());
}

// Answers are kept per (host, title): the same title again costs nothing,
// also when Claude gave none.
TEST_F(ShortTitlerTest, CachesAnswers) {
  TurnOn();
  Serve(R"({"titles": {"1": "PR #123: fix login"}})");
  AddTab("https://github.com/org/repo/pull/123", kPrTitle);
  AddTab("https://www.youtube.com/watch?v=1", kVideoTitle);
  task_environment_.FastForwardBy(ShortTitler::kBatchDelay);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(requests_.size(), 1u);

  SidebarNode* again =
      AddTab("https://github.com/org/repo/pull/123/files", kPrTitle);
  EXPECT_EQ(again->short_title(), u"PR #123: fix login");
  AddTab("https://www.youtube.com/watch?v=2", kVideoTitle);
  // Another site's same title is another title.
  SidebarNode* elsewhere = AddTab("https://mirror.test/pr", kPrTitle);
  EXPECT_TRUE(elsewhere->short_title().empty());
  EXPECT_EQ(titler_->pending_for_testing(), 1u);
  task_environment_.FastForwardBy(ShortTitler::kBatchDelay);
  ASSERT_EQ(requests_.size(), 2u);
  EXPECT_NE(UserMessage(1).find("mirror.test"), std::string::npos);
  EXPECT_EQ(UserMessage(1).find("youtube"), std::string::npos);
}

// A new title drops the short one, which was for the old, and asks again;
// a name the user gives meanwhile wins.
TEST_F(ShortTitlerTest, TitleChangesAndCustomTitles) {
  TurnOn();
  Serve(R"({"titles": {"1": "PR #123: fix login"}})");
  SidebarNode* pr = AddTab("https://github.com/org/repo/pull/123", kPrTitle);
  task_environment_.FastForwardBy(ShortTitler::kBatchDelay);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(pr->short_title(), u"PR #123: fix login");

  space_.SetTitle(pr, u"Issue #9: crash on start \u00B7 org/repo");
  EXPECT_TRUE(pr->short_title().empty());
  EXPECT_EQ(titler_->pending_for_testing(), 1u);
  // Named by the user before the batch goes: it's left out.
  space_.SetCustomTitle(pr, u"My bug");
  EXPECT_EQ(titler_->pending_for_testing(), 0u);
  task_environment_.FastForwardBy(ShortTitler::kBatchDelay);
  EXPECT_EQ(requests_.size(), 1u);

  // Back to the page's title: the cache answers for the old one.
  space_.SetCustomTitle(pr, std::u16string());
  space_.SetTitle(pr, kPrTitle);
  EXPECT_EQ(pr->short_title(), u"PR #123: fix login");
  EXPECT_EQ(requests_.size(), 1u);
}

// At most kMaxBatch titles a request; that many go at once, the rest after.
TEST_F(ShortTitlerTest, MaxBatch) {
  TurnOn();
  Serve(R"({"titles": {}})");
  for (size_t i = 0; i < ShortTitler::kMaxBatch + 3; ++i) {
    AddTab("https://site" + base::NumberToString(i) + ".test/",
           u"A long title for page number " + base::NumberToString16(i) +
               u" of the site");
  }
  task_environment_.RunUntilIdle();
  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_NE(UserMessage(0).find("20. "), std::string::npos);
  EXPECT_EQ(UserMessage(0).find("21. "), std::string::npos);
  EXPECT_EQ(titler_->pending_for_testing(), 3u);
  task_environment_.FastForwardBy(ShortTitler::kBatchDelay);
  ASSERT_EQ(requests_.size(), 2u);
  EXPECT_NE(UserMessage(1).find("3. "), std::string::npos);
  EXPECT_EQ(UserMessage(1).find("4. "), std::string::npos);
}

// Turning the switch off takes every short title away; on again, they come
// back from the cache without a request.
TEST_F(ShortTitlerTest, SwitchOffClears) {
  TurnOn();
  Serve(R"({"titles": {"1": "PR #123: fix login"}})");
  SidebarNode* pr = AddTab("https://github.com/org/repo/pull/123", kPrTitle);
  task_environment_.FastForwardBy(ShortTitler::kBatchDelay);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(pr->short_title(), u"PR #123: fix login");

  prefs_.SetBoolean(claude::prefs::kShortTabTitles, false);
  EXPECT_TRUE(pr->short_title().empty());
  AddTab("https://www.youtube.com/watch?v=1", kVideoTitle);
  task_environment_.FastForwardBy(base::Minutes(1));
  EXPECT_EQ(requests_.size(), 1u);

  TurnOn();
  EXPECT_EQ(pr->short_title(), u"PR #123: fix login");
}

// Pinned tabs get them too; favorites, whose tiles show no title, don't.
TEST_F(ShortTitlerTest, PinnedAndFavorites) {
  TurnOn();
  Serve(R"({"titles": {"1": "PR #123: fix login"}})");
  SpaceModel favorites(SpaceModel::Kind::kFavorites);
  titler_->Observe(favorites);
  SidebarNode* pinned = space_.AddTab(space_.pinned_root(), 0,
                                      GURL("https://github.com/o/r/pull/123"),
                                      kPrTitle);
  favorites.AddTab(favorites.pinned_root(), 0,
                   GURL("https://www.youtube.com/watch?v=1"), kVideoTitle);
  EXPECT_EQ(titler_->pending_for_testing(), 1u);
  task_environment_.FastForwardBy(ShortTitler::kBatchDelay);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_EQ(UserMessage(0).find("youtube"), std::string::npos);
  EXPECT_EQ(pinned->short_title(), u"PR #123: fix login");
  titler_->Shutdown();
}

}  // namespace
}  // namespace compass
