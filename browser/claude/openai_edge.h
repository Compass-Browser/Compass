// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_OPENAI_EDGE_H_
#define COMPASS_BROWSER_CLAUDE_OPENAI_EDGE_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "compass/browser/claude/ai_provider.h"
#include "compass/browser/claude/sse_parser.h"

// Other providers at the edge (docs/design/ai-providers.md): Compass builds
// and reads the Messages API everywhere; a request to OpenAI becomes a
// Responses API request, to Gemini or another server a Chat Completions one,
// just before it's sent, and their streams become the Messages events they
// stand for, so nothing past ClaudeClient knows which provider answered.
// iOS: CompassClaude's OpenAIChat, OpenAIResponses and OpenAIEdge.
namespace compass::claude::openai {

// The system prompt's first words, and what they become.
inline constexpr char kClaudeIdentity[] = "You are Claude,";
inline constexpr char kNeutralIdentity[] = "You are an AI assistant,";
// What a tool result says when it had images, which go in a user message
// after the results (neither API takes an image in a tool result).
inline constexpr char kImageFollows[] = "(The screenshot follows.)";
inline constexpr char kImagesNote[] =
    "The screenshots from the tool results above:";

// The system blocks' text, joined by a blank line; Claude's name made
// neutral.
std::string SystemText(const base::Value* system);

// "low" for the reasoning models that take it (OpenAI's gpt-5, gpt-6 and o
// series, Gemini 2.5 and later); nullopt for the others, which may refuse
// the field.
std::optional<std::string> LightEffort(std::string_view model,
                                       AIProvider provider);

// A Messages API body as a Chat Completions body for |provider|.
// |json_reply|: the reply must be one JSON object (the sidebar's requests).
// A body without "thinking" is a light request (the sidebar's, the command
// bar's), which asks for little reasoning where the provider knows the
// field. No max_tokens: the chat's is past most models' limits, and a small
// one is spent on reasoning first.
base::DictValue ChatCompletionsRequest(const base::DictValue& body,
                                       AIProvider provider,
                                       bool json_reply);

// The same as a Responses API body (OpenAI), stateless (store: false).
base::DictValue ResponsesRequest(const base::DictValue& body, bool json_reply);

// The one |provider| gets.
base::DictValue TranslateRequest(const base::DictValue& body,
                                 AIProvider provider,
                                 bool json_reply);

// Turns a Chat Completions stream's chunks into the Messages API's events.
class ChatStream {
 public:
  ChatStream();
  ChatStream(const ChatStream&) = delete;
  ChatStream& operator=(const ChatStream&) = delete;
  ~ChatStream();

  // The events for one "data:" payload.
  std::vector<SseParser::Event> Feed(std::string_view data);
  // The stream ended: the closing events if it had a finish reason, else
  // none (the reply ended early).
  std::vector<SseParser::Event> Finish();
  // What went wrong, from an error chunk.
  const std::optional<std::string>& error() const { return error_; }

 private:
  void Write(std::string_view kind,
             base::DictValue delta,
             std::vector<SseParser::Event>& events);
  void ToolDelta(const base::DictValue& call,
                 std::vector<SseParser::Event>& events);
  void CloseOpen(std::vector<SseParser::Event>& events);
  std::vector<SseParser::Event> End();

  // The block being written to, and its kind ("text", "thinking").
  std::optional<int> open_index_;
  std::string open_kind_;
  // Tool calls' blocks, by the call's index (or id), in order.
  std::map<std::string, int> tools_;
  std::vector<int> tool_order_;
  int next_index_ = 0;
  std::optional<std::string> finish_reason_;
  bool ended_ = false;
  std::optional<std::string> error_;
};

// Turns a Responses stream's events into the Messages API's.
class ResponsesStream {
 public:
  ResponsesStream();
  ResponsesStream(const ResponsesStream&) = delete;
  ResponsesStream& operator=(const ResponsesStream&) = delete;
  ~ResponsesStream();

  std::vector<SseParser::Event> Feed(std::string_view data);
  const std::optional<std::string>& error() const { return error_; }

 private:
  struct Call {
    int block;
    bool streamed;
  };
  // The block for |key|, started with |block| if new.
  int Open(const std::string& key,
           base::DictValue block,
           std::vector<SseParser::Event>& events);
  std::vector<SseParser::Event> End(std::string_view reason);

  std::map<std::string, int> blocks_;
  std::map<int, Call> calls_;
  std::vector<int> order_;
  int next_index_ = 0;
  bool ended_ = false;
  std::optional<std::string> error_;
};

// The translator |provider| gets.
class StreamTranslator {
 public:
  explicit StreamTranslator(AIProvider provider);
  StreamTranslator(const StreamTranslator&) = delete;
  StreamTranslator& operator=(const StreamTranslator&) = delete;
  ~StreamTranslator();

  std::vector<SseParser::Event> Feed(std::string_view data);
  std::vector<SseParser::Event> Finish();
  const std::optional<std::string>& error() const;

 private:
  const bool responses_;
  ChatStream chat_;
  ResponsesStream responses_stream_;
};

}  // namespace compass::claude::openai

#endif  // COMPASS_BROWSER_CLAUDE_OPENAI_EDGE_H_
