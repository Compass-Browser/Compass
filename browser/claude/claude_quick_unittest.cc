// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_quick.h"

#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_request.h"
#include "components/prefs/testing_pref_service.h"
#include "net/base/net_errors.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

constexpr char kKey[] = "sk-ant-api03-quick-test-key";
constexpr char kMessagesUrl[] = "https://api.anthropic.com/v1/messages";

// A streamed reply whose text is |text|, ending with |stop_reason|.
std::string Reply(std::string_view text,
                  std::string_view stop_reason = "end_turn") {
  std::string escaped;
  for (char c : text) {
    if (c == '\n') {
      escaped += "\\n";
      continue;
    }
    if (c == '"' || c == '\\') {
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
       "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"",
       stop_reason,
       "\"}}\n\n"
       "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n"});
}

class ClaudeQuickTest : public testing::Test {
 protected:
  ClaudeQuickTest() {
    prefs::RegisterProfilePrefs(prefs_.registry());
    prefs_.SetString(prefs::kApiKey, "encrypted");
    prefs_.SetBoolean(prefs::kNameLinkFolders, true);
    factory_.SetInterceptor(base::BindRepeating(
        [](std::vector<network::ResourceRequest>* requests,
           const network::ResourceRequest& request) {
          requests->push_back(request);
        },
        base::Unretained(&requests_)));
    quick_ = std::make_unique<ClaudeQuick>(
        &prefs_,
        base::BindRepeating(
            [](network::TestURLLoaderFactory* factory)
                -> scoped_refptr<network::SharedURLLoaderFactory> {
              return base::MakeRefCounted<
                  network::WeakWrapperSharedURLLoaderFactory>(factory);
            },
            base::Unretained(&factory_)),
        base::BindRepeating(
            [](std::string* key, ClaudeQuick::KeyCallback callback) {
              std::move(callback).Run(*key);
            },
            base::Unretained(&key_)));
  }

  ClaudeQuick::Result Ask(QuickRequest request = QuickRequest()) {
    if (request.feature_pref.empty()) {
      request.feature_pref = prefs::kNameLinkFolders;
    }
    if (request.system.empty()) {
      request.system = "Name the folder.";
      request.user = "- Tokio docs (tokio.rs)";
    }
    base::test::TestFuture<ClaudeQuick::Result> future;
    quick_->Ask(std::move(request), future.GetCallback());
    return future.Take();
  }

  // The JSON body of the last request sent.
  base::DictValue LastBody() {
    EXPECT_FALSE(requests_.empty());
    if (requests_.empty()) {
      return base::DictValue();
    }
    const std::string body = network::GetUploadData(requests_.back());
    return base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC)
        .value_or(base::DictValue());
  }

  void Serve(const std::string& body, int status = net::HTTP_OK) {
    factory_.AddResponse(kMessagesUrl, body,
                         static_cast<net::HttpStatusCode>(status));
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  TestingPrefServiceSimple prefs_;
  network::TestURLLoaderFactory factory_;
  std::vector<network::ResourceRequest> requests_;
  std::string key_ = kKey;
  std::unique_ptr<ClaudeQuick> quick_;
};

TEST_F(ClaudeQuickTest, RequestShape) {
  Serve(Reply("{\"name\": \"Rust Async\"}"));
  QuickRequest request;
  request.system = "Name the folder.";
  request.user = "- Tokio (tokio.rs)";
  request.max_tokens = 5000;
  ClaudeQuick::Result result = Ask(request);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result->FindString("name"), "Rust Async");

  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_EQ(requests_[0].url, GURL(kMessagesUrl));
  EXPECT_EQ(requests_[0].headers.GetHeader("x-api-key"), kKey);
  base::DictValue body = LastBody();
  EXPECT_EQ(*body.FindString("model"), kDefaultQuickModel);
  EXPECT_EQ(*body.FindInt("max_tokens"), ClaudeQuick::kMaxTokens);
  EXPECT_FALSE(body.Find("tools"));
  EXPECT_FALSE(body.Find("thinking"));
  EXPECT_EQ(body.FindBool("stream"), true);
  const base::ListValue* system = body.FindList("system");
  ASSERT_TRUE(system);
  ASSERT_EQ(system->size(), 1u);
  EXPECT_EQ(*(*system)[0].GetDict().FindString("text"), "Name the folder.");
  EXPECT_TRUE((*system)[0].GetDict().Find("cache_control"));
  const base::ListValue* messages = body.FindList("messages");
  ASSERT_TRUE(messages);
  ASSERT_EQ(messages->size(), 1u);
  EXPECT_EQ(*(*messages)[0].GetDict().FindString("role"), "user");
  EXPECT_NE(base::WriteJson(*messages)->find("Tokio"), std::string::npos);
}

// The feature names its model; the chat's cache level applies.
TEST_F(ClaudeQuickTest, FeatureModelAndCacheLevel) {
  prefs_.SetString(prefs::kBaseUrl, "https://gw.example.com");
  RememberCacheLevel(prefs_, GURL("https://gw.example.com/"),
                     CacheLevel::kNone);
  factory_.AddResponse("https://gw.example.com/v1/messages",
                       Reply("{\"name\": \"X\"}"));
  QuickRequest request;
  request.model = "claude-sonnet-5-5";
  ASSERT_TRUE(Ask(request).has_value());
  base::DictValue body = LastBody();
  EXPECT_EQ(*body.FindString("model"), "claude-sonnet-5-5");
  EXPECT_EQ(CountCacheBreakpoints(base::Value(body.Clone())), 0);
}

TEST_F(ClaudeQuickTest, SubscriptionTokenSaysItIsClaudeCode) {
  key_ = "sk-ant-oat01-token";
  // Refused until its risks are accepted, before anything is sent.
  EXPECT_EQ(Ask().error(), QuickError::kNotSetUp);
  EXPECT_TRUE(requests_.empty());
  prefs_.SetBoolean(prefs::kAcceptTokenRisks, true);
  Serve(Reply("{\"name\": \"X\"}"));
  ASSERT_TRUE(Ask().has_value());
  const base::DictValue body = LastBody();
  const base::ListValue* system = body.FindList("system");
  ASSERT_TRUE(system);
  ASSERT_EQ(system->size(), 2u);
  EXPECT_EQ(*(*system)[0].GetDict().FindString("text"), kOAuthSystemLine);
}

TEST_F(ClaudeQuickTest, OffOrNotSetUpSendsNothing) {
  // Each feature asks only while its own switch is on.
  prefs_.SetBoolean(prefs::kNameLinkFolders, false);
  EXPECT_FALSE(quick_->IsEnabled(prefs::kNameLinkFolders));
  EXPECT_EQ(Ask().error(), QuickError::kOff);
  QuickRequest no_switch;
  no_switch.feature_pref = "compass.claude.no_such_switch";
  no_switch.system = "s";
  no_switch.user = "u";
  EXPECT_EQ(Ask(no_switch).error(), QuickError::kOff);
  prefs_.SetBoolean(prefs::kNameLinkFolders, true);
  EXPECT_TRUE(quick_->IsEnabled(prefs::kNameLinkFolders));
  prefs_.SetString(prefs::kApiKey, "");
  EXPECT_FALSE(quick_->IsSetUp());
  EXPECT_EQ(Ask().error(), QuickError::kNotSetUp);
  EXPECT_TRUE(requests_.empty());
}

TEST_F(ClaudeQuickTest, ParsesOnlyOneJsonObject) {
  Serve(Reply("  {\"name\": \"Trip\"}\n"));
  EXPECT_TRUE(Ask().has_value());
  Serve(Reply("Sure! {\"name\": \"Trip\"}"));
  EXPECT_EQ(Ask().error(), QuickError::kBadReply);
  Serve(Reply("{\"name\": \"Trip\"} and more"));
  EXPECT_EQ(Ask().error(), QuickError::kBadReply);
  Serve(Reply("[\"Trip\"]"));
  EXPECT_EQ(Ask().error(), QuickError::kBadReply);

  EXPECT_TRUE(ParseQuickReply("{\"a\": 1}"));
  EXPECT_FALSE(ParseQuickReply("```json\n{\"a\": 1}\n```"));
  EXPECT_FALSE(ParseQuickReply(""));
}

TEST_F(ClaudeQuickTest, Refusal) {
  Serve(Reply("", "refusal"));
  EXPECT_EQ(Ask().error(), QuickError::kRefused);
  // Not the endpoint's fault: no back-off.
  Serve(Reply("{\"name\": \"X\"}"));
  EXPECT_TRUE(Ask().has_value());
}

TEST_F(ClaudeQuickTest, TimesOut) {
  // No response: the request hangs.
  base::test::TestFuture<ClaudeQuick::Result> future;
  QuickRequest request;
  request.feature_pref = prefs::kNameLinkFolders;
  request.system = "s";
  request.user = "u";
  quick_->Ask(request, future.GetCallback());
  task_environment_.FastForwardBy(ClaudeQuick::kTimeout -
                                  base::Milliseconds(1));
  EXPECT_FALSE(future.IsReady());
  task_environment_.FastForwardBy(base::Milliseconds(2));
  ASSERT_TRUE(future.IsReady());
  EXPECT_EQ(future.Get().error(), QuickError::kTimeout);
}

TEST_F(ClaudeQuickTest, RateLimit) {
  for (size_t i = 0; i < ClaudeQuick::kMaxBackgroundPerHour; ++i) {
    Serve(Reply("{\"name\": \"X\"}"));
    ASSERT_TRUE(Ask().has_value()) << i;
  }
  EXPECT_EQ(Ask().error(), QuickError::kRateLimited);
  EXPECT_EQ(requests_.size(), ClaudeQuick::kMaxBackgroundPerHour);
  // One the user asked for still goes.
  QuickRequest asked;
  asked.feature_pref = prefs::kNameLinkFolders;
  asked.system = "s";
  asked.user = "u";
  asked.background = false;
  EXPECT_TRUE(Ask(asked).has_value());
  // An hour after the first, there's room again.
  task_environment_.FastForwardBy(base::Hours(1));
  EXPECT_EQ(quick_->RecentBackgroundRequests(), 0u);
  EXPECT_TRUE(Ask().has_value());
}

TEST_F(ClaudeQuickTest, BacksOffAfterErrors) {
  Serve("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
        "\"message\":\"Overloaded\"}}",
        529);
  EXPECT_EQ(Ask().error(), QuickError::kFailed);
  // A minute of waiting, without asking.
  const size_t sent = requests_.size();
  EXPECT_EQ(Ask().error(), QuickError::kBackingOff);
  EXPECT_EQ(requests_.size(), sent);
  task_environment_.FastForwardBy(ClaudeQuick::kFirstBackOff);
  // Fails again: ten minutes.
  factory_.AddResponse(GURL(kMessagesUrl),
                       network::mojom::URLResponseHead::New(), "",
                       network::URLLoaderCompletionStatus(net::ERR_FAILED));
  EXPECT_EQ(Ask().error(), QuickError::kFailed);
  task_environment_.FastForwardBy(ClaudeQuick::kFirstBackOff);
  EXPECT_EQ(Ask().error(), QuickError::kBackingOff);
  task_environment_.FastForwardBy(ClaudeQuick::kLaterBackOff);
  // Works: the next error waits a minute again.
  Serve(Reply("{\"name\": \"X\"}"));
  EXPECT_TRUE(Ask().has_value());
}

TEST_F(ClaudeQuickTest, SanitizeAiName) {
  auto name = [](std::string_view text) {
    std::optional<std::u16string> result = SanitizeAiName(text);
    return result ? base::UTF16ToUTF8(*result) : std::string("(dropped)");
  };
  EXPECT_EQ(name("Rust Async Runtimes"), "Rust Async Runtimes");
  EXPECT_EQ(name("  \"Flights to Lisbon.\" "), "Flights to Lisbon");
  EXPECT_EQ(name("\xe2\x80\x9cVoli  per\tLisbona\xe2\x80\x9d"),
            "Voli per Lisbona");
  EXPECT_EQ(name("React"), "React");
  EXPECT_EQ(name("\xe6\x97\x85\xe8\xa1\x8c"), "\xe6\x97\x85\xe8\xa1\x8c");
  EXPECT_EQ(name(""), "(dropped)");
  EXPECT_EQ(name("   "), "(dropped)");
  EXPECT_EQ(name("..."), "(dropped)");
  EXPECT_EQ(name("Four Words Are Many"), "(dropped)");
  EXPECT_EQ(name("Supercalifragilisticexpialidocious"), "(dropped)");
  EXPECT_EQ(name("Trip \xe2\x9c\x88\xef\xb8\x8f"), "(dropped)");
  EXPECT_EQ(name("Party \xf0\x9f\x8e\x89"), "(dropped)");
  EXPECT_EQ(name("Line\nBreak"), "(dropped)");
  EXPECT_EQ(name("<b>Bold</b>"), "(dropped)");
  EXPECT_EQ(name("{\"name\": \"X\"}"), "(dropped)");
  EXPECT_EQ(name("A\xe2\x80\xae" "B"), "(dropped)");
  EXPECT_EQ(name("\xff\xfe"), "(dropped)");
}

TEST_F(ClaudeQuickTest, SidebarPrompts) {
  constexpr char kPrompts[] =
      "# Prompts\n\nIntro, not sent.\n\n"
      "## folder_name\n\nName it, in {locale}.\n\n"
      "## tidy_today  \nGroup them.\n";
  EXPECT_EQ(SidebarPromptFrom(kPrompts, "folder_name", "it-IT"),
            "Name it, in it-IT.");
  EXPECT_EQ(SidebarPromptFrom(kPrompts, "tidy_today", "en"), "Group them.");
  EXPECT_EQ(SidebarPromptFrom(kPrompts, "short_titles", "en"), "");
  // The file built into the browser has the folder names' prompt, asking
  // for JSON.
  const std::string built = SidebarPrompt("folder_name", "en-US");
  EXPECT_NE(built.find("{\"name\""), std::string::npos);
  EXPECT_NE(built.find("en-US"), std::string::npos);
  EXPECT_EQ(built.find("{locale}"), std::string::npos);
}

}  // namespace
}  // namespace compass::claude
