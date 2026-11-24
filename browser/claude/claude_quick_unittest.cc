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
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "compass/browser/claude/ai_provider.h"
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
#include "testing/gmock/include/gmock/gmock.h"
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

// Tidy Today's reply (docs/design/ai-sidebar.md): folders of 2 or more
// known entries, each in one folder, with names that pass, at most 6.
TEST_F(ClaudeQuickTest, ParseTidyReply) {
  const std::vector<std::string> ids = {"1", "2", "3", "4", "5",
                                        "6", "7", "8", "9", "10",
                                        "11", "12", "13", "14"};
  auto parse = [&](std::string_view json) {
    std::optional<base::DictValue> reply = ParseQuickReply(json);
    EXPECT_TRUE(reply) << json;
    const base::DictValue empty;
    std::vector<std::string> folders;
    for (const TidyFolder& folder :
         ParseTidyReply(reply ? *reply : empty, ids)) {
      folders.push_back(base::UTF16ToUTF8(folder.name) + ":" +
                        base::JoinString(folder.entries, ","));
    }
    return folders;
  };
  using ::testing::ElementsAre;
  using ::testing::IsEmpty;

  EXPECT_THAT(parse(R"({"folders": [
      {"name": "Rust Async", "entries": ["1", "4"]},
      {"name": "Trip to Lisbon", "entries": ["2", "3", "5"]}]})"),
              ElementsAre("Rust Async:1,4", "Trip to Lisbon:2,3,5"));
  EXPECT_THAT(parse(R"({"folders": []})"), IsEmpty());
  // Another shape: nothing.
  EXPECT_THAT(parse(R"({"name": "X"})"), IsEmpty());
  EXPECT_THAT(parse(R"({"folders": {"name": "X"}})"), IsEmpty());

  // Unknown ids, and ids repeated in a folder or taken by an earlier one,
  // are dropped; a folder left with one entry goes, and frees it.
  EXPECT_THAT(parse(R"({"folders": [
      {"name": "Docs", "entries": ["1", "99", "1", "2", "x", null]},
      {"name": "Mail", "entries": ["2", "3"]},
      {"name": "News", "entries": ["3", "4", "2"]}]})"),
              ElementsAre("Docs:1,2", "News:3,4"));
  // A number for an id is taken as one.
  EXPECT_THAT(parse(R"({"folders": [{"name": "Docs", "entries": [5, 6]}]})"),
              ElementsAre("Docs:5,6"));

  // Singletons and folders without a name that passes are dropped.
  EXPECT_THAT(parse(R"({"folders": [
      {"name": "Alone", "entries": ["1"]},
      {"name": "Four Words Are Many", "entries": ["2", "3"]},
      {"name": "", "entries": ["4", "5"]},
      {"entries": ["6", "7"]},
      {"name": "Party \ud83c\udf89", "entries": ["8", "9"]},
      {"name": "\"Recipes.\"", "entries": ["10", "11"]}]})"),
              ElementsAre("Recipes:10,11"));

  // At most six folders: the first six that pass.
  EXPECT_THAT(parse(R"({"folders": [
      {"name": "Bad", "entries": ["1"]},
      {"name": "A", "entries": ["1", "2"]},
      {"name": "B", "entries": ["3", "4"]},
      {"name": "C", "entries": ["5", "6"]},
      {"name": "D", "entries": ["7", "8"]},
      {"name": "E", "entries": ["9", "10"]},
      {"name": "F", "entries": ["11", "12"]},
      {"name": "G", "entries": ["13", "14"]}]})"),
              ElementsAre("A:1,2", "B:3,4", "C:5,6", "D:7,8", "E:9,10",
                          "F:11,12"));
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
  // And Tidy Today's.
  const std::string tidy = SidebarPrompt("tidy_today", "it-IT");
  EXPECT_NE(tidy.find("{\"folders\""), std::string::npos);
  EXPECT_NE(tidy.find("it-IT"), std::string::npos);
  EXPECT_EQ(tidy.find("## "), std::string::npos);
  // And Shorter Tab Titles'.
  const std::string titles = SidebarPrompt("short_titles", "en");
  EXPECT_NE(titles.find("{\"titles\""), std::string::npos);
  EXPECT_EQ(titles.find("## "), std::string::npos);
}

// The on-device prompts (docs/design/ai-sidebar.md, "On this device"): the
// language spelled out, and a feature without its own section uses
// Claude's.
TEST_F(ClaudeQuickTest, OnDevicePrompts) {
  EXPECT_EQ(SidebarPromptFrom("## a\nIn {language}.\n", "a", "it-IT"),
            "In Italian.");
  EXPECT_EQ(SidebarPromptFrom("## a\nIn {language}.\n", "a", "pt_BR"),
            "In Portuguese.");

  QuickRequest folder;
  SetOnDevicePrompt(folder, "folder_name", "it-IT");
  EXPECT_EQ(folder.on_device_feature, "folder_name");
  EXPECT_EQ(folder.on_device_system,
            SidebarPrompt("folder_name_on_device", "it-IT"));
  EXPECT_NE(folder.on_device_system.find("Italian"), std::string::npos);
  EXPECT_EQ(folder.on_device_system.find("{language}"), std::string::npos);

  QuickRequest tidy;
  SetOnDevicePrompt(tidy, "tidy_today", "en");
  EXPECT_EQ(tidy.on_device_system, SidebarPrompt("tidy_today_on_device", "en"));
  EXPECT_FALSE(tidy.on_device_system.empty());

  QuickRequest none;
  SetOnDevicePrompt(none, "command_bar_ask", "en");
  EXPECT_EQ(none.on_device_system, SidebarPrompt("command_bar_ask", "en"));
}

// Shorter tab titles: which titles get one.
TEST_F(ClaudeQuickTest, NeedsShortTitle) {
  EXPECT_FALSE(NeedsShortTitle(u""));
  EXPECT_FALSE(NeedsShortTitle(u"Tokio docs"));
  EXPECT_FALSE(NeedsShortTitle(u"A title of exactly forty characters long"));
  EXPECT_TRUE(
      NeedsShortTitle(u"A title of a bit more than forty characters"));
  // Site suffixes, however short the title.
  EXPECT_TRUE(NeedsShortTitle(u"Pull request #123 \u00B7 org/repo"));
  EXPECT_TRUE(NeedsShortTitle(u"Cats - YouTube"));
  EXPECT_TRUE(NeedsShortTitle(u"Review | The Verge"));
  EXPECT_TRUE(NeedsShortTitle(u"Home \u2014 Site"));
  // A dash inside a word, or nothing before the separator, isn't one.
  EXPECT_FALSE(NeedsShortTitle(u"Well-known facts"));
  EXPECT_FALSE(NeedsShortTitle(u" - YouTube"));
}

// Short titles may be phrases of up to 40 characters, with ids and colons,
// but nothing a name refuses.
TEST_F(ClaudeQuickTest, SanitizeShortTitle) {
  EXPECT_EQ(SanitizeShortTitle("PR #123: fix login"), u"PR #123: fix login");
  EXPECT_EQ(SanitizeShortTitle("  \"Invoice  2026-031.\" "),
            u"Invoice 2026-031");
  EXPECT_EQ(SanitizeShortTitle("Five words are fine as well"),
            u"Five words are fine as well");
  EXPECT_EQ(SanitizeShortTitle(std::string(40, 'a')), std::u16string(40, 'a'));
  EXPECT_FALSE(SanitizeShortTitle(std::string(41, 'a')));
  EXPECT_FALSE(SanitizeShortTitle(""));
  EXPECT_FALSE(SanitizeShortTitle("..."));
  EXPECT_FALSE(SanitizeShortTitle("Line\nbreak"));
  EXPECT_FALSE(SanitizeShortTitle("<b>Bold</b>"));
  EXPECT_FALSE(SanitizeShortTitle("Fox \xF0\x9F\xA6\x8A"));
}

TEST_F(ClaudeQuickTest, ParseShortTitlesReply) {
  const std::vector<std::string> ids = {"1", "2", "3", "4"};
  std::optional<base::DictValue> reply = ParseQuickReply(
      R"({"titles": {"1": "PR #123: fix login", "2": "", "3": "<b>x</b>",
          "9": "Unknown", "4": 4}})");
  ASSERT_TRUE(reply);
  const std::map<std::string, std::u16string> titles =
      ParseShortTitlesReply(*reply, ids);
  ASSERT_EQ(titles.size(), 1u);
  EXPECT_EQ(titles.at("1"), u"PR #123: fix login");
  EXPECT_TRUE(ParseShortTitlesReply(base::DictValue(), ids).empty());
  std::optional<base::DictValue> list = ParseQuickReply(R"({"titles": []})");
  ASSERT_TRUE(list);
  EXPECT_TRUE(ParseShortTitlesReply(*list, ids).empty());
}

// Ask in the command bar: what reads as a question.
TEST_F(ClaudeQuickTest, LooksLikeQuestion) {
  EXPECT_TRUE(LooksLikeQuestion(u"is it raining in Lisbon?", "en-US"));
  EXPECT_TRUE(LooksLikeQuestion(u"  what is a fox  ", "en-US"));
  EXPECT_TRUE(LooksLikeQuestion(u"What's the capital of Peru", "en-US"));
  EXPECT_TRUE(LooksLikeQuestion(u"How do foxes hunt", "it-IT"));
  EXPECT_TRUE(LooksLikeQuestion(u"cosa mangiano le volpi", "it-IT"));
  EXPECT_TRUE(LooksLikeQuestion(u"Perch\u00E9 il cielo \u00E8 blu", "it"));
  EXPECT_TRUE(LooksLikeQuestion(u"por qu\u00E9 llueve", "es-419"));
  EXPECT_TRUE(LooksLikeQuestion(u"\u00BFllueve hoy en Madrid", "en-US"));
  EXPECT_TRUE(LooksLikeQuestion(u"wie sp\u00E4t ist es", "de-DE"));
  // Another language's words count only in that language.
  EXPECT_FALSE(LooksLikeQuestion(u"cosa mangiano le volpi", "en-US"));
  EXPECT_FALSE(LooksLikeQuestion(u"come together lyrics", "en-US"));
  // Searches, URLs and single words aren't questions.
  EXPECT_FALSE(LooksLikeQuestion(u"fox facts", "en-US"));
  EXPECT_FALSE(LooksLikeQuestion(u"what?", "en-US"));
  EXPECT_FALSE(LooksLikeQuestion(u"https://example.com/?q=what is", "en-US"));
  EXPECT_FALSE(LooksLikeQuestion(u"", "en-US"));
  EXPECT_FALSE(LooksLikeQuestion(u"whatever works", "en-US"));
}

// "About this page" is the tab's title and host, on one line.
TEST_F(ClaudeQuickTest, DescribeAskPage) {
  EXPECT_EQ(DescribeAskPage("", ""), "");
  EXPECT_EQ(DescribeAskPage("Fox\npage", "fox.test"),
            "I'm looking at a page titled \"Fox page\" on fox.test.");
  EXPECT_EQ(DescribeAskPage("", "fox.test"),
            "I'm looking at a page on fox.test.");
}

// Ask's answer streams in as plain text, on its own switch and model, with
// no tools and no hourly limit.
TEST_F(ClaudeQuickTest, StreamsAnswers) {
  prefs_.SetBoolean(prefs::kCommandBarAsk, true);
  Serve(Reply("Foxes are small canids.\nThey eat mice."));
  QuickRequest request =
      MakeAskRequest("what is a fox?", DescribeAskPage("Fox page", "fox.test"),
                     "claude-sonnet-5-5", "en-US");
  EXPECT_EQ(request.feature_pref, prefs::kCommandBarAsk);
  EXPECT_FALSE(request.background);
  std::vector<std::string> texts;
  base::test::TestFuture<ClaudeQuick::Result> done;
  quick_->Stream(std::move(request),
                 base::BindLambdaForTesting(
                     [&](const std::string& text) { texts.push_back(text); }),
                 done.GetCallback());
  ClaudeQuick::Result result = done.Take();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result->FindString("text"),
            "Foxes are small canids.\nThey eat mice.");
  EXPECT_THAT(texts,
              testing::ElementsAre("Foxes are small canids.\nThey eat mice."));
  EXPECT_EQ(quick_->RecentBackgroundRequests(), 0u);

  base::DictValue body = LastBody();
  EXPECT_EQ(*body.FindString("model"), "claude-sonnet-5-5");
  EXPECT_EQ(*body.FindInt("max_tokens"), ClaudeQuick::kMaxTokens);
  EXPECT_FALSE(body.Find("tools"));
  EXPECT_FALSE(body.Find("thinking"));
  const base::ListValue* system = body.FindList("system");
  ASSERT_TRUE(system);
  EXPECT_NE((*system)[0].GetDict().FindString("text")->find("command bar"),
            std::string::npos);
  const base::ListValue* messages = body.FindList("messages");
  ASSERT_TRUE(messages);
  ASSERT_EQ(messages->size(), 1u);
  const base::ListValue* content =
      (*messages)[0].GetDict().FindList("content");
  ASSERT_TRUE(content);
  EXPECT_EQ(*(*content)[0].GetDict().FindString("text"),
            "I'm looking at a page titled \"Fox page\" on fox.test.\n\n"
            "what is a fox?");
}

// docs/design/ai-providers.md: the sidebar's requests on Gemini go as Chat
// Completions, with its key, its sidebar model and a JSON reply asked for;
// the streamed chunks come back as the reply's text.
TEST_F(ClaudeQuickTest, OnGemini) {
  prefs_.SetString(prefs::kProvider, "gemini");
  prefs_.SetString(prefs::ProviderPref(prefs::kApiKey, AIProvider::kGemini),
                   "encrypted");
  key_ = "AIza-test";
  constexpr char kUrl[] =
      "https://generativelanguage.googleapis.com/v1beta/openai/"
      "chat/completions";
  factory_.AddResponse(
      kUrl,
      "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\","
      "\"content\":\"{\\\"name\\\": \"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"\\\"Rust Async\\\"}\"},"
      "\"finish_reason\":\"stop\"}]}\n\n"
      "data: [DONE]\n\n");
  QuickRequest request;
  request.feature_pref = prefs::kNameLinkFolders;
  request.model = prefs_.GetString(
      prefs::CurrentPref(prefs_, prefs::kNameLinkFoldersModel));
  ClaudeQuick::Result result = Ask(request);
  ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
  EXPECT_EQ(*result->FindString("name"), "Rust Async");

  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_EQ(requests_[0].url, GURL(kUrl));
  EXPECT_EQ(requests_[0].headers.GetHeader("Authorization"),
            "Bearer AIza-test");
  EXPECT_FALSE(requests_[0].headers.HasHeader("x-api-key"));
  base::DictValue body = LastBody();
  EXPECT_EQ(*body.FindString("model"), "gemini-2.5-flash");
  EXPECT_EQ(*body.FindStringByDottedPath("response_format.type"),
            "json_object");
  EXPECT_EQ(*body.FindString("reasoning_effort"), "low");
  EXPECT_FALSE(body.Find("max_tokens"));
  EXPECT_FALSE(body.Find("cache_control"));
  EXPECT_FALSE(body.Find("system"));
}

// On OpenAI: the Responses API, Luna by default.
TEST_F(ClaudeQuickTest, OnOpenAI) {
  prefs_.SetString(prefs::kProvider, "openai");
  prefs_.SetString(prefs::ProviderPref(prefs::kApiKey, AIProvider::kOpenAI),
                   "encrypted");
  key_ = "sk-test";
  constexpr char kUrl[] = "https://api.openai.com/v1/responses";
  factory_.AddResponse(
      kUrl,
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,"
      "\"delta\":\"{\\\"name\\\": \\\"Rust Async\\\"}\"}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":"
      "{\"status\":\"completed\"}}\n\n");
  ClaudeQuick::Result result = Ask();
  ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
  EXPECT_EQ(*result->FindString("name"), "Rust Async");
  base::DictValue body = LastBody();
  EXPECT_EQ(*body.FindString("model"), "gpt-6-luna");
  EXPECT_EQ(*body.FindStringByDottedPath("reasoning.effort"), "low");
  EXPECT_EQ(*body.FindStringByDottedPath("text.format.type"), "json_object");
  EXPECT_EQ(body.FindBool("store"), false);
}

// An OpenAI-compatible server may take no key, and its sidebar requests use
// the chat's model.
TEST_F(ClaudeQuickTest, OnCompatibleServerWithoutKey) {
  prefs_.SetString(prefs::kProvider, "compatible");
  EXPECT_FALSE(quick_->IsSetUp());
  prefs_.SetString(
      prefs::ProviderPref(prefs::kBaseUrl, AIProvider::kCompatible),
      "http://localhost:11434/v1");
  prefs_.SetString(prefs::ProviderPref(prefs::kModel, AIProvider::kCompatible),
                   "llama4");
  EXPECT_TRUE(quick_->IsSetUp());
  key_.clear();
  constexpr char kUrl[] = "http://localhost:11434/v1/chat/completions";
  factory_.AddResponse(
      kUrl,
      "data: {\"choices\":[{\"delta\":{\"content\":\"{\\\"name\\\": "
      "\\\"Rust\\\"}\"},\"finish_reason\":\"stop\"}]}\n\n"
      "data: [DONE]\n\n");
  ClaudeQuick::Result result = Ask();
  ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
  EXPECT_EQ(*result->FindString("name"), "Rust");
  EXPECT_FALSE(requests_.back().headers.HasHeader("Authorization"));
  base::DictValue body = LastBody();
  EXPECT_EQ(*body.FindString("model"), "llama4");
  EXPECT_FALSE(body.Find("response_format"));
}

// Nothing is sent with the switch off; a cancelled answer calls nothing.
TEST_F(ClaudeQuickTest, StreamOffAndCancel) {
  base::test::TestFuture<ClaudeQuick::Result> off;
  quick_->Stream(MakeAskRequest("what is a fox?", "", "", "en"),
                 base::DoNothing(), off.GetCallback());
  EXPECT_EQ(off.Take().error(), QuickError::kOff);
  EXPECT_TRUE(requests_.empty());

  prefs_.SetBoolean(prefs::kCommandBarAsk, true);
  Serve(Reply("An answer."));
  bool called = false;
  const int id = quick_->Stream(
      MakeAskRequest("what is a fox?", "", "", "en"),
      base::BindLambdaForTesting([&](const std::string&) { called = true; }),
      base::BindLambdaForTesting(
          [&](ClaudeQuick::Result) { called = true; }));
  quick_->Cancel(id);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(called);
  // The prompt asks for plain text, not JSON.
  EXPECT_NE(SidebarPrompt("command_bar_ask", "en").find("Plain text"),
            std::string::npos);
}

}  // namespace
}  // namespace compass::claude
