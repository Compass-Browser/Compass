// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/openai_edge.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "compass/browser/claude/claude_request.h"
#include "testing/gtest/include/gtest/gtest.h"

// docs/design/ai-providers.md: the Messages API as Chat Completions and the
// Responses API, and back. iOS: OpenAIChatTests, OpenAIResponsesTests.
namespace compass::claude::openai {
namespace {

base::DictValue Dict(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  EXPECT_TRUE(dict) << json;
  return dict ? std::move(*dict) : base::DictValue();
}

base::Value Parse(std::string_view json) {
  return base::JSONReader::Read(json, base::JSON_PARSE_RFC)
      .value_or(base::Value());
}

// A chat round as the chat page keeps it: the user's message, Claude's
// thinking, text and a screenshot call, then its result (with the image)
// and an error for another call.
base::ListValue ChatMessages() {
  return std::move(Parse(R"([
    {"role": "user", "content": [{"type": "text", "text": "Click it"}]},
    {"role": "assistant", "content": [
      {"type": "thinking", "thinking": "Plan", "signature": "sig"},
      {"type": "text", "text": "Looking."},
      {"type": "tool_use", "id": "toolu_1", "name": "screenshot",
       "input": {"full": true}}]},
    {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "toolu_1", "content": [
        {"type": "text", "text": "The page."},
        {"type": "image", "source": {"type": "base64",
                                     "media_type": "image/jpeg",
                                     "data": "QUJD"}}]},
      {"type": "tool_result", "tool_use_id": "toolu_2",
       "content": [{"type": "text", "text": "No such tab."}],
       "is_error": true},
      {"type": "text", "text": "And now?"}]}
  ])")).TakeList();
}

// The chat's own request, as BuildChatRequest() makes it.
base::DictValue ChatBody(std::string_view model) {
  ChatOptions options;
  options.model = std::string(model);
  options.claude_api = false;
  ChatRequest request = BuildChatRequest(ChatMessages(), options);
  request.body.Set("stream", true);
  return std::move(request.body);
}

// A sidebar request: no thinking, no tools.
base::DictValue QuickBody(std::string_view model) {
  return Dict(base::StrCat({R"({"model": ")", model, R"(",
    "max_tokens": 400, "cache_control": {"type": "ephemeral"},
    "system": [{"type": "text", "text": "Name it.",
                "cache_control": {"type": "ephemeral"}}],
    "messages": [{"role": "user",
                  "content": [{"type": "text", "text": "Tabs"}]}]})"}));
}

std::vector<std::string> Roles(const base::ListValue& messages) {
  std::vector<std::string> roles;
  for (const base::Value& message : messages) {
    const std::string* role = message.GetDict().FindString("role");
    const std::string* type = message.GetDict().FindString("type");
    roles.push_back(role ? *role : (type ? *type : "?"));
  }
  return roles;
}

TEST(OpenAIEdgeTest, ChatCompletionsRequest) {
  base::DictValue body = ChatCompletionsRequest(
      ChatBody("gemini-2.5-pro"), AIProvider::kGemini, /*json_reply=*/false);
  EXPECT_EQ(*body.FindString("model"), "gemini-2.5-pro");
  EXPECT_EQ(body.FindBool("stream"), true);
  // Anthropic's fields are gone.
  for (const char* field :
       {"system", "thinking", "max_tokens", "cache_control", "fallbacks"}) {
    EXPECT_FALSE(body.Find(field)) << field;
  }
  // The chat thinks: no light effort.
  EXPECT_FALSE(body.Find("reasoning_effort"));

  const base::ListValue& messages = *body.FindList("messages");
  EXPECT_EQ(Roles(messages), (std::vector<std::string>{
                                 "system", "user", "assistant", "tool",
                                 "tool", "user"}));
  // The system prompt, joined, with a neutral name.
  const std::string& system = *messages[0].GetDict().FindString("content");
  EXPECT_TRUE(system.starts_with("You are an AI assistant, built into"))
      << system.substr(0, 80);
  EXPECT_EQ(*messages[1].GetDict().FindString("content"), "Click it");
  // The assistant: its text, no thinking, the call with its arguments.
  const base::DictValue& assistant = messages[2].GetDict();
  EXPECT_EQ(*assistant.FindString("content"), "Looking.");
  const base::DictValue& call =
      assistant.FindList("tool_calls")->front().GetDict();
  EXPECT_EQ(*call.FindString("id"), "toolu_1");
  EXPECT_EQ(*call.FindStringByDottedPath("function.name"), "screenshot");
  EXPECT_EQ(Parse(*call.FindStringByDottedPath("function.arguments")),
            Parse(R"({"full": true})"));
  // The results: text and the image's note, the error.
  EXPECT_EQ(*messages[3].GetDict().FindString("tool_call_id"), "toolu_1");
  EXPECT_EQ(*messages[3].GetDict().FindString("content"),
            base::StrCat({"The page.\n", kImageFollows}));
  EXPECT_EQ(*messages[4].GetDict().FindString("content"),
            "Error: No such tab.");
  // The screenshot after the tool messages, as a data URL, then the text.
  const base::ListValue& parts = *messages[5].GetDict().FindList("content");
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(*parts[0].GetDict().FindString("text"), kImagesNote);
  EXPECT_EQ(*parts[1].GetDict().FindStringByDottedPath("image_url.url"),
            "data:image/jpeg;base64,QUJD");
  EXPECT_EQ(*parts[2].GetDict().FindString("text"), "And now?");
  // Tools as functions.
  const base::ListValue* tools = body.FindList("tools");
  ASSERT_TRUE(tools && !tools->empty());
  const base::DictValue& tool = tools->front().GetDict();
  EXPECT_EQ(*tool.FindString("type"), "function");
  EXPECT_TRUE(tool.FindStringByDottedPath("function.name"));
  EXPECT_TRUE(tool.FindDictByDottedPath("function.parameters"));
}

TEST(OpenAIEdgeTest, LightRequestsAskForLittleReasoning) {
  base::DictValue openai = ChatCompletionsRequest(
      QuickBody("gpt-5-mini"), AIProvider::kOpenAI, /*json_reply=*/true);
  EXPECT_EQ(*openai.FindString("reasoning_effort"), "low");
  EXPECT_EQ(*openai.FindStringByDottedPath("response_format.type"),
            "json_object");
  EXPECT_EQ(Roles(*openai.FindList("messages")),
            (std::vector<std::string>{"system", "user"}));
  EXPECT_FALSE(openai.Find("stream"));
  EXPECT_FALSE(openai.Find("max_tokens"));
  // Gemini 2.5 takes it; an older model or another server doesn't get it.
  EXPECT_EQ(*ChatCompletionsRequest(QuickBody("gemini-2.5-flash"),
                                    AIProvider::kGemini, false)
                 .FindString("reasoning_effort"),
            "low");
  EXPECT_FALSE(ChatCompletionsRequest(QuickBody("gpt-4.1"),
                                      AIProvider::kOpenAI, false)
                   .Find("reasoning_effort"));
  base::DictValue other = ChatCompletionsRequest(
      QuickBody("gpt-5"), AIProvider::kCompatible, /*json_reply=*/true);
  EXPECT_FALSE(other.Find("reasoning_effort"));
  EXPECT_FALSE(other.Find("response_format"));
}

TEST(OpenAIEdgeTest, ResponsesRequest) {
  base::DictValue body = TranslateRequest(
      ChatBody("gpt-6.1-terra"), AIProvider::kOpenAI, /*json_reply=*/false);
  EXPECT_EQ(*body.FindString("model"), "gpt-6.1-terra");
  EXPECT_EQ(body.FindBool("stream"), true);
  EXPECT_EQ(body.FindBool("store"), false);
  EXPECT_TRUE(
      body.FindString("instructions")->starts_with("You are an AI assistant,"));
  for (const char* field : {"system", "messages", "thinking", "max_tokens",
                            "cache_control", "reasoning"}) {
    EXPECT_FALSE(body.Find(field)) << field;
  }
  const base::ListValue& input = *body.FindList("input");
  EXPECT_EQ(Roles(input), (std::vector<std::string>{
                              "user", "assistant", "function_call",
                              "function_call_output", "function_call_output",
                              "user"}));
  EXPECT_EQ(*input[0].GetDict().FindString("content"), "Click it");
  EXPECT_EQ(*input[1].GetDict().FindString("content"), "Looking.");
  EXPECT_EQ(*input[2].GetDict().FindString("call_id"), "toolu_1");
  EXPECT_EQ(Parse(*input[2].GetDict().FindString("arguments")),
            Parse(R"({"full": true})"));
  EXPECT_EQ(*input[3].GetDict().FindString("output"),
            base::StrCat({"The page.\n", kImageFollows}));
  EXPECT_EQ(*input[4].GetDict().FindString("output"), "Error: No such tab.");
  const base::ListValue& parts = *input[5].GetDict().FindList("content");
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(*parts[0].GetDict().FindString("type"), "input_text");
  EXPECT_EQ(*parts[1].GetDict().FindString("type"), "input_image");
  EXPECT_EQ(*parts[1].GetDict().FindString("image_url"),
            "data:image/jpeg;base64,QUJD");
  EXPECT_EQ(*parts[2].GetDict().FindString("text"), "And now?");
  const base::DictValue& tool = body.FindList("tools")->front().GetDict();
  EXPECT_EQ(*tool.FindString("type"), "function");
  EXPECT_TRUE(tool.FindString("name"));
  EXPECT_TRUE(tool.FindDict("parameters"));

  base::DictValue quick =
      TranslateRequest(QuickBody("gpt-6-luna"), AIProvider::kOpenAI, true);
  EXPECT_EQ(*quick.FindStringByDottedPath("reasoning.effort"), "low");
  EXPECT_EQ(*quick.FindStringByDottedPath("text.format.type"), "json_object");
  EXPECT_EQ(*quick.FindString("instructions"), "Name it.");
}

// A reply put together from Messages events, as the chat page does.
struct Reply {
  std::map<int, base::DictValue> blocks;
  std::map<int, std::string> json;
  std::vector<std::string> names;
  std::string stop_reason;

  void Apply(const std::vector<SseParser::Event>& events) {
    for (const SseParser::Event& event : events) {
      names.push_back(event.name);
      base::DictValue data = Dict(event.data);
      EXPECT_EQ(*data.FindString("type"), event.name);
      const int index = data.FindInt("index").value_or(-1);
      if (event.name == "content_block_start") {
        blocks[index] = data.FindDict("content_block")->Clone();
      } else if (event.name == "content_block_delta") {
        const base::DictValue& delta = *data.FindDict("delta");
        const std::string& type = *delta.FindString("type");
        base::DictValue& block = blocks.at(index);
        if (type == "text_delta") {
          block.Set("text", *block.FindString("text") +
                                *delta.FindString("text"));
        } else if (type == "thinking_delta") {
          block.Set("thinking", *block.FindString("thinking") +
                                    *delta.FindString("thinking"));
        } else if (type == "input_json_delta") {
          json[index] += *delta.FindString("partial_json");
        }
      } else if (event.name == "content_block_stop") {
        if (json.contains(index)) {
          blocks.at(index).Set("input", Parse(json[index]));
        }
      } else if (event.name == "message_delta") {
        stop_reason = *data.FindStringByDottedPath("delta.stop_reason");
      }
    }
  }

  std::vector<std::string> Types() const {
    std::vector<std::string> types;
    for (const auto& [index, block] : blocks) {
      types.push_back(*block.FindString("type"));
    }
    return types;
  }
};

Reply Stream(AIProvider provider,
             const std::vector<std::string>& chunks,
             std::optional<std::string>* error = nullptr) {
  StreamTranslator translator(provider);
  Reply reply;
  for (const std::string& chunk : chunks) {
    reply.Apply(translator.Feed(chunk));
  }
  reply.Apply(translator.Finish());
  if (error) {
    *error = translator.error();
  }
  return reply;
}

TEST(OpenAIEdgeTest, ChatStreamsText) {
  std::optional<std::string> error;
  Reply reply = Stream(
      AIProvider::kGemini,
      {R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})",
       R"({"choices":[{"index":0,"delta":{"content":"Hel"}}]})",
       R"({"choices":[{"index":0,"delta":{"content":"lo."}}]})",
       R"({"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})",
       R"({"choices":[],"usage":{"total_tokens":5}})", "[DONE]"},
      &error);
  EXPECT_FALSE(error);
  EXPECT_EQ(reply.names.back(), "message_stop");
  EXPECT_EQ(reply.Types(), std::vector<std::string>{"text"});
  EXPECT_EQ(*reply.blocks[0].FindString("text"), "Hello.");
  EXPECT_EQ(reply.stop_reason, "end_turn");
}

TEST(OpenAIEdgeTest, ChatStreamsReasoningThenToolCalls) {
  // Calls by index, arguments in pieces, two calls interleaved.
  Reply reply = Stream(
      AIProvider::kCompatible,
      {R"({"choices":[{"delta":{"reasoning_content":"Plan."}}]})",
       R"({"choices":[{"delta":{"content":"On it."}}]})",
       R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function","function":{"name":"click","arguments":""}}]}}]})",
       R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"ref\":"}}]}}]})",
       R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function","function":{"name":"read_page","arguments":"{}"}}]}}]})",
       R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"e1\"}"}}]}}]})",
       R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})",
       "[DONE]"});
  EXPECT_EQ(reply.Types(), (std::vector<std::string>{"thinking", "text",
                                                     "tool_use", "tool_use"}));
  EXPECT_EQ(*reply.blocks[0].FindString("thinking"), "Plan.");
  EXPECT_EQ(*reply.blocks[2].FindString("id"), "call_a");
  EXPECT_EQ(*reply.blocks[2].Find("input"), Parse(R"({"ref":"e1"})"));
  EXPECT_EQ(*reply.blocks[3].FindString("name"), "read_page");
  EXPECT_EQ(*reply.blocks[3].Find("input"), Parse("{}"));
  EXPECT_EQ(reply.stop_reason, "tool_use");
}

TEST(OpenAIEdgeTest, GeminiCallsWithoutIndexOrIdAndStop) {
  // Whole calls without an index or id, "stop" after them, and no [DONE].
  Reply reply = Stream(
      AIProvider::kGemini,
      {R"({"choices":[{"delta":{"role":"assistant","tool_calls":[{"function":{"name":"find","arguments":"{\"query\":\"login\"}"},"type":"function"}]}}]})",
       R"({"choices":[{"delta":{"tool_calls":[{"id":"","function":{"name":"read_page","arguments":"{}"},"type":"function"}]},"finish_reason":"stop"}]})"});
  ASSERT_EQ(reply.blocks.size(), 2u);
  EXPECT_EQ(*reply.blocks[0].FindString("name"), "find");
  EXPECT_EQ(*reply.blocks[1].FindString("name"), "read_page");
  // Ids made up, different, kept for the results.
  const std::string& first = *reply.blocks[0].FindString("id");
  const std::string& second = *reply.blocks[1].FindString("id");
  EXPECT_NE(first, second);
  EXPECT_TRUE(first.starts_with("call_"));
  EXPECT_TRUE(second.starts_with("call_"));
  EXPECT_EQ(*reply.blocks[0].Find("input"), Parse(R"({"query":"login"})"));
  EXPECT_EQ(reply.stop_reason, "tool_use");
  EXPECT_EQ(reply.names.back(), "message_stop");
}

TEST(OpenAIEdgeTest, ChatStopReasonsErrorsAndEarlyEnds) {
  EXPECT_EQ(
      Stream(AIProvider::kGemini,
             {R"({"choices":[{"delta":{"content":"a"},"finish_reason":"length"}]})",
              "[DONE]"})
          .stop_reason,
      "max_tokens");
  EXPECT_EQ(
      Stream(AIProvider::kGemini,
             {R"({"choices":[{"delta":{},"finish_reason":"content_filter"}]})",
              "[DONE]"})
          .stop_reason,
      "refusal");
  std::optional<std::string> error;
  Stream(AIProvider::kGemini,
         {R"({"error":{"message":"Quota exceeded","type":"insufficient_quota"}})"},
         &error);
  EXPECT_EQ(error, "Quota exceeded");
  // No finish reason and no [DONE]: no message_stop (the reply ended early).
  Reply half =
      Stream(AIProvider::kGemini, {R"({"choices":[{"delta":{"content":"Half"}}]})"});
  EXPECT_FALSE(std::ranges::contains(half.names, "message_stop"));
}

TEST(OpenAIEdgeTest, ResponsesStreamsReasoningTextAndCalls) {
  std::optional<std::string> error;
  Reply reply = Stream(
      AIProvider::kOpenAI,
      {R"({"type":"response.created","response":{"id":"resp_1","status":"in_progress"}})",
       R"({"type":"response.output_item.added","output_index":0,"item":{"type":"reasoning","id":"rs_1"}})",
       R"({"type":"response.reasoning_summary_text.delta","output_index":0,"summary_index":0,"delta":"Plan."})",
       R"({"type":"response.output_item.added","output_index":1,"item":{"type":"message","id":"msg_1","role":"assistant"}})",
       R"({"type":"response.output_text.delta","output_index":1,"content_index":0,"delta":"On "})",
       R"({"type":"response.output_text.delta","output_index":1,"content_index":0,"delta":"it."})",
       R"({"type":"response.output_item.added","output_index":2,"item":{"type":"function_call","id":"fc_1","call_id":"call_a","name":"click","arguments":""}})",
       R"({"type":"response.function_call_arguments.delta","output_index":2,"delta":"{\"ref\":"})",
       R"({"type":"response.function_call_arguments.delta","output_index":2,"delta":"\"e1\"}"})",
       R"({"type":"response.output_item.done","output_index":2,"item":{"type":"function_call","call_id":"call_a","name":"click","arguments":"{\"ref\":\"e1\"}"}})",
       // A call whose arguments come only whole.
       R"({"type":"response.output_item.added","output_index":3,"item":{"type":"function_call","call_id":"call_b","name":"read_page","arguments":""}})",
       R"({"type":"response.output_item.done","output_index":3,"item":{"type":"function_call","call_id":"call_b","name":"read_page","arguments":"{\"filter\":\"interactive\"}"}})",
       R"({"type":"response.completed","response":{"id":"resp_1","status":"completed"}})"},
      &error);
  EXPECT_FALSE(error);
  EXPECT_EQ(reply.Types(), (std::vector<std::string>{"thinking", "text",
                                                     "tool_use", "tool_use"}));
  EXPECT_EQ(*reply.blocks[0].FindString("thinking"), "Plan.");
  EXPECT_EQ(*reply.blocks[1].FindString("text"), "On it.");
  // Arguments once, not twice (deltas and the item's own).
  EXPECT_EQ(*reply.blocks[2].FindString("id"), "call_a");
  EXPECT_EQ(*reply.blocks[2].Find("input"), Parse(R"({"ref":"e1"})"));
  EXPECT_EQ(*reply.blocks[3].FindString("id"), "call_b");
  EXPECT_EQ(*reply.blocks[3].Find("input"),
            Parse(R"({"filter":"interactive"})"));
  EXPECT_EQ(reply.stop_reason, "tool_use");
}

TEST(OpenAIEdgeTest, ResponsesEndsAndFailures) {
  EXPECT_EQ(
      Stream(AIProvider::kOpenAI,
             {R"({"type":"response.output_text.delta","output_index":0,"delta":"Hi"})",
              R"({"type":"response.completed","response":{"status":"completed"}})"})
          .stop_reason,
      "end_turn");
  EXPECT_EQ(
      Stream(AIProvider::kOpenAI,
             {R"({"type":"response.output_text.delta","output_index":0,"delta":"Hi"})",
              R"({"type":"response.incomplete","response":{"incomplete_details":{"reason":"max_output_tokens"}}})"})
          .stop_reason,
      "max_tokens");
  std::optional<std::string> error;
  Stream(AIProvider::kOpenAI,
         {R"({"type":"response.failed","response":{"error":{"code":"server_error","message":"Boom"}}})"},
         &error);
  EXPECT_EQ(error, "Boom");
  Stream(AIProvider::kOpenAI,
         {R"({"type":"error","code":"rate_limit_exceeded","message":"Slow down"})"},
         &error);
  EXPECT_EQ(error, "Slow down");
  // No response.completed: no stop (the reply ended early).
  Reply half = Stream(
      AIProvider::kOpenAI,
      {R"({"type":"response.output_text.delta","output_index":0,"delta":"Half"})"});
  EXPECT_TRUE(half.stop_reason.empty());
  EXPECT_FALSE(std::ranges::contains(half.names, "message_stop"));
}

}  // namespace
}  // namespace compass::claude::openai
