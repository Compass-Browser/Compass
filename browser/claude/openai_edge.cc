// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/openai_edge.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "compass/browser/claude/chatgpt_oauth.h"

namespace compass::claude::openai {

namespace {

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("{}");
}

SseParser::Event MakeEvent(std::string_view name, base::DictValue data) {
  data.Set("type", name);
  return SseParser::Event{std::string(name), Json(base::Value(std::move(data)))};
}

base::DictValue TextBlock(std::string_view text) {
  base::DictValue block;
  block.Set("type", "text");
  block.Set("text", text);
  return block;
}

// A message's (or tool result's) content as blocks: a string is one text
// block.
base::ListValue Blocks(const base::Value* content) {
  base::ListValue blocks;
  if (!content) {
    return blocks;
  }
  if (content->is_string()) {
    blocks.Append(TextBlock(content->GetString()));
  } else if (content->is_list()) {
    for (const base::Value& block : content->GetList()) {
      blocks.Append(block.Clone());
    }
  }
  return blocks;
}

std::string TypeOf(const base::Value& block) {
  const base::DictValue* dict = block.GetIfDict();
  const std::string* type = dict ? dict->FindString("type") : nullptr;
  return type ? *type : std::string();
}

std::string StringOr(const base::DictValue& dict,
                     std::string_view key,
                     std::string_view fallback = "") {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string(fallback);
}

// A tool_use block's input as a JSON string.
std::string Arguments(const base::DictValue& block) {
  const base::Value* input = block.Find("input");
  return input ? Json(*input) : "{}";
}

// A base64 image block as an image_url part with a data URL.
std::optional<base::DictValue> ImagePart(const base::Value& value) {
  const base::DictValue* block = value.GetIfDict();
  if (!block || TypeOf(value) != "image") {
    return std::nullopt;
  }
  const base::DictValue* source = block->FindDict("source");
  const std::string* data = source ? source->FindString("data") : nullptr;
  if (!data || StringOr(*source, "type") != "base64") {
    return std::nullopt;
  }
  base::DictValue url;
  url.Set("url", base::StrCat({"data:",
                               StringOr(*source, "media_type", "image/jpeg"),
                               ";base64,", *data}));
  base::DictValue part;
  part.Set("type", "image_url");
  part.Set("image_url", std::move(url));
  return part;
}

// The same as an input_image part (Responses).
std::optional<base::DictValue> InputImagePart(const base::Value& value) {
  std::optional<base::DictValue> part = ImagePart(value);
  if (!part) {
    return std::nullopt;
  }
  base::DictValue out;
  out.Set("type", "input_image");
  out.Set("image_url", *part->FindStringByDottedPath("image_url.url"));
  return out;
}

// A tool_result's text for a tool message or a function call output, and
// its images into |images|.
std::string ToolResultText(const base::DictValue& result,
                           bool responses,
                           base::ListValue& images) {
  std::vector<std::string> texts;
  bool had_images = false;
  for (const base::Value& inner : Blocks(result.Find("content"))) {
    if (TypeOf(inner) == "text") {
      if (const std::string* text = inner.GetDict().FindString("text")) {
        texts.push_back(*text);
      }
      continue;
    }
    std::optional<base::DictValue> image =
        responses ? InputImagePart(inner) : ImagePart(inner);
    if (image) {
      images.Append(std::move(*image));
      had_images = true;
    }
  }
  std::string text = base::JoinString(texts, "\n");
  if (had_images) {
    text += text.empty() ? kImageFollows : base::StrCat({"\n", kImageFollows});
  }
  if (result.FindBool("is_error").value_or(false)) {
    text = base::StrCat({"Error: ", text});
  }
  return text;
}

// A user message of |parts|: a plain string when they're all text (every
// server takes that).
base::DictValue UserMessage(base::ListValue parts, std::string_view text_type) {
  bool all_text = true;
  std::vector<std::string> texts;
  for (const base::Value& part : parts) {
    if (TypeOf(part) != text_type) {
      all_text = false;
      break;
    }
    texts.push_back(StringOr(part.GetDict(), "text"));
  }
  base::DictValue message;
  message.Set("role", "user");
  if (all_text) {
    message.Set("content", base::JoinString(texts, "\n\n"));
  } else {
    message.Set("content", std::move(parts));
  }
  return message;
}

// The assistant's text blocks, joined.
std::string AssistantText(const base::ListValue& blocks) {
  std::string text;
  for (const base::Value& block : blocks) {
    if (TypeOf(block) == "text") {
      text += StringOr(block.GetDict(), "text");
    }
  }
  return text;
}

// One Messages API message as Chat Completions messages, appended to |out|:
// an assistant message with its tool calls; or a user message's tool
// results as tool messages, then its text and images (the results' images
// too) as a user message.
void ConvertForChat(const base::DictValue& message, base::ListValue& out) {
  const base::ListValue blocks = Blocks(message.Find("content"));
  if (StringOr(message, "role", "user") == "assistant") {
    const std::string text = AssistantText(blocks);
    base::ListValue calls;
    for (const base::Value& block : blocks) {
      if (TypeOf(block) != "tool_use") {
        continue;
      }
      const base::DictValue& dict = block.GetDict();
      base::DictValue function;
      function.Set("name", StringOr(dict, "name"));
      function.Set("arguments", Arguments(dict));
      base::DictValue call;
      call.Set("id", StringOr(dict, "id"));
      call.Set("type", "function");
      call.Set("function", std::move(function));
      calls.Append(std::move(call));
    }
    if (text.empty() && calls.empty()) {
      return;
    }
    base::DictValue assistant;
    assistant.Set("role", "assistant");
    if (text.empty()) {
      assistant.Set("content", base::Value());
    } else {
      assistant.Set("content", text);
    }
    if (!calls.empty()) {
      assistant.Set("tool_calls", std::move(calls));
    }
    out.Append(std::move(assistant));
    return;
  }
  base::ListValue parts;
  base::ListValue result_images;
  for (const base::Value& block : blocks) {
    const std::string type = TypeOf(block);
    if (type == "tool_result") {
      const base::DictValue& dict = block.GetDict();
      base::DictValue tool;
      tool.Set("role", "tool");
      tool.Set("tool_call_id", StringOr(dict, "tool_use_id"));
      tool.Set("content",
               ToolResultText(dict, /*responses=*/false, result_images));
      out.Append(std::move(tool));
    } else if (type == "text") {
      parts.Append(TextBlock(StringOr(block.GetDict(), "text")));
    } else if (std::optional<base::DictValue> image = ImagePart(block)) {
      parts.Append(std::move(*image));
    }
  }
  if (!result_images.empty()) {
    base::ListValue all;
    all.Append(TextBlock(kImagesNote));
    for (base::Value& image : result_images) {
      all.Append(std::move(image));
    }
    for (base::Value& part : parts) {
      all.Append(std::move(part));
    }
    parts = std::move(all);
  }
  if (!parts.empty()) {
    out.Append(UserMessage(std::move(parts), "text"));
  }
}

// The same as Responses input items.
void ConvertForResponses(const base::DictValue& message,
                         base::ListValue& out) {
  const base::ListValue blocks = Blocks(message.Find("content"));
  if (StringOr(message, "role", "user") == "assistant") {
    const std::string text = AssistantText(blocks);
    if (!text.empty()) {
      base::DictValue assistant;
      assistant.Set("role", "assistant");
      assistant.Set("content", text);
      out.Append(std::move(assistant));
    }
    for (const base::Value& block : blocks) {
      if (TypeOf(block) != "tool_use") {
        continue;
      }
      const base::DictValue& dict = block.GetDict();
      base::DictValue call;
      call.Set("type", "function_call");
      call.Set("call_id", StringOr(dict, "id"));
      call.Set("name", StringOr(dict, "name"));
      call.Set("arguments", Arguments(dict));
      out.Append(std::move(call));
    }
    return;
  }
  base::ListValue parts;
  base::ListValue result_images;
  for (const base::Value& block : blocks) {
    const std::string type = TypeOf(block);
    if (type == "tool_result") {
      const base::DictValue& dict = block.GetDict();
      base::DictValue output;
      output.Set("type", "function_call_output");
      output.Set("call_id", StringOr(dict, "tool_use_id"));
      output.Set("output",
                 ToolResultText(dict, /*responses=*/true, result_images));
      out.Append(std::move(output));
    } else if (type == "text") {
      base::DictValue part;
      part.Set("type", "input_text");
      part.Set("text", StringOr(block.GetDict(), "text"));
      parts.Append(std::move(part));
    } else if (std::optional<base::DictValue> image = InputImagePart(block)) {
      parts.Append(std::move(*image));
    }
  }
  if (!result_images.empty()) {
    base::ListValue all;
    base::DictValue note;
    note.Set("type", "input_text");
    note.Set("text", kImagesNote);
    all.Append(std::move(note));
    for (base::Value& image : result_images) {
      all.Append(std::move(image));
    }
    for (base::Value& part : parts) {
      all.Append(std::move(part));
    }
    parts = std::move(all);
  }
  if (!parts.empty()) {
    out.Append(UserMessage(std::move(parts), "input_text"));
  }
}

// The definition's input_schema, or an empty object schema.
base::Value Parameters(const base::DictValue& definition) {
  if (const base::Value* schema = definition.Find("input_schema")) {
    return schema->Clone();
  }
  base::DictValue empty;
  empty.Set("type", "object");
  empty.Set("properties", base::DictValue());
  return base::Value(std::move(empty));
}

bool WantsStream(const base::DictValue& body) {
  return body.FindBool("stream").value_or(false);
}

base::DictValue JsonObjectFormat() {
  base::DictValue format;
  format.Set("type", "json_object");
  return format;
}

}  // namespace

std::string SystemText(const base::Value* system) {
  std::string text;
  if (system && system->is_string()) {
    text = system->GetString();
  } else if (system && system->is_list()) {
    std::vector<std::string> texts;
    for (const base::Value& block : system->GetList()) {
      const base::DictValue* dict = block.GetIfDict();
      const std::string* part = dict ? dict->FindString("text") : nullptr;
      if (part && !part->empty()) {
        texts.push_back(*part);
      }
    }
    text = base::JoinString(texts, "\n\n");
  }
  if (text.starts_with(kClaudeIdentity)) {
    text = base::StrCat(
        {kNeutralIdentity,
         std::string_view(text).substr(std::string_view(kClaudeIdentity).size())});
  }
  return text;
}

std::optional<std::string> LightEffort(std::string_view model,
                                       AIProvider provider) {
  std::string id = base::ToLowerASCII(model);
  if (id.starts_with("models/")) {
    id = id.substr(7);
  }
  switch (provider) {
    case AIProvider::kOpenAI: {
      const bool reasoning =
          id.starts_with("gpt-5") || id.starts_with("gpt-6") ||
          (id.size() > 1 && id[0] == 'o' && base::IsAsciiDigit(id[1]));
      return reasoning ? std::optional<std::string>("low") : std::nullopt;
    }
    case AIProvider::kGemini:
      return id.starts_with("gemini-2.5") || id.starts_with("gemini-3")
                 ? std::optional<std::string>("low")
                 : std::nullopt;
    case AIProvider::kAnthropic:
    case AIProvider::kCompatible:
      return std::nullopt;
  }
  return std::nullopt;
}

base::DictValue ChatCompletionsRequest(const base::DictValue& body,
                                       AIProvider provider,
                                       bool json_reply) {
  const std::string model = StringOr(body, "model");
  base::ListValue messages;
  const std::string system = SystemText(body.Find("system"));
  if (!system.empty()) {
    base::DictValue message;
    message.Set("role", "system");
    message.Set("content", system);
    messages.Append(std::move(message));
  }
  if (const base::ListValue* list = body.FindList("messages")) {
    for (const base::Value& message : *list) {
      if (message.is_dict()) {
        ConvertForChat(message.GetDict(), messages);
      }
    }
  }
  base::DictValue out;
  out.Set("model", model);
  out.Set("messages", std::move(messages));
  if (WantsStream(body)) {
    out.Set("stream", true);
  }
  base::ListValue tools;
  if (const base::ListValue* definitions = body.FindList("tools")) {
    for (const base::Value& definition : *definitions) {
      const base::DictValue* dict = definition.GetIfDict();
      const std::string* name = dict ? dict->FindString("name") : nullptr;
      if (!name) {
        continue;
      }
      base::DictValue function;
      function.Set("name", *name);
      if (const std::string* description = dict->FindString("description")) {
        function.Set("description", *description);
      }
      function.Set("parameters", Parameters(*dict));
      base::DictValue tool;
      tool.Set("type", "function");
      tool.Set("function", std::move(function));
      tools.Append(std::move(tool));
    }
  }
  if (!tools.empty()) {
    out.Set("tools", std::move(tools));
  }
  if (!body.Find("thinking")) {
    if (std::optional<std::string> effort = LightEffort(model, provider)) {
      out.Set("reasoning_effort", *effort);
    }
  }
  if (json_reply &&
      (provider == AIProvider::kOpenAI || provider == AIProvider::kGemini)) {
    out.Set("response_format", JsonObjectFormat());
  }
  return out;
}

base::DictValue ResponsesRequest(const base::DictValue& body, bool json_reply) {
  const std::string model = StringOr(body, "model");
  base::ListValue input;
  if (const base::ListValue* list = body.FindList("messages")) {
    for (const base::Value& message : *list) {
      if (message.is_dict()) {
        ConvertForResponses(message.GetDict(), input);
      }
    }
  }
  base::DictValue out;
  out.Set("model", model);
  out.Set("input", std::move(input));
  out.Set("store", false);
  const std::string instructions = SystemText(body.Find("system"));
  if (!instructions.empty()) {
    out.Set("instructions", instructions);
  }
  if (WantsStream(body)) {
    out.Set("stream", true);
  }
  base::ListValue tools;
  if (const base::ListValue* definitions = body.FindList("tools")) {
    for (const base::Value& definition : *definitions) {
      const base::DictValue* dict = definition.GetIfDict();
      const std::string* name = dict ? dict->FindString("name") : nullptr;
      if (!name) {
        continue;
      }
      base::DictValue tool;
      tool.Set("type", "function");
      tool.Set("name", *name);
      if (const std::string* description = dict->FindString("description")) {
        tool.Set("description", *description);
      }
      tool.Set("parameters", Parameters(*dict));
      tools.Append(std::move(tool));
    }
  }
  if (!tools.empty()) {
    out.Set("tools", std::move(tools));
  }
  if (!body.Find("thinking")) {
    if (std::optional<std::string> effort =
            LightEffort(model, AIProvider::kOpenAI)) {
      base::DictValue reasoning;
      reasoning.Set("effort", *effort);
      out.Set("reasoning", std::move(reasoning));
    }
  }
  if (json_reply) {
    base::DictValue text;
    text.Set("format", JsonObjectFormat());
    out.Set("text", std::move(text));
  }
  return out;
}

base::DictValue TranslateRequest(const base::DictValue& body,
                                 AIProvider provider,
                                 bool json_reply) {
  return UsesResponsesApi(provider)
             ? ResponsesRequest(body, json_reply)
             : ChatCompletionsRequest(body, provider, json_reply);
}

// ChatStream

ChatStream::ChatStream() = default;
ChatStream::~ChatStream() = default;

std::vector<SseParser::Event> ChatStream::Feed(std::string_view data) {
  std::vector<SseParser::Event> events;
  if (ended_) {
    return events;
  }
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(data, base::TRIM_ALL);
  if (trimmed == "[DONE]") {
    return End();
  }
  std::optional<base::DictValue> chunk =
      base::JSONReader::ReadDict(trimmed, base::JSON_PARSE_RFC);
  if (!chunk) {
    return events;
  }
  if (const base::Value* failure = chunk->Find("error");
      failure && !failure->is_none()) {
    const std::string* message =
        failure->is_dict() ? failure->GetDict().FindString("message")
                           : (failure->is_string() ? &failure->GetString()
                                                   : nullptr);
    error_ = message && !message->empty() ? *message : "The reply failed.";
    ended_ = true;
    return events;
  }
  const base::ListValue* choices = chunk->FindList("choices");
  if (!choices || choices->empty() || !choices->front().is_dict()) {
    // Usage and other chunks without choices.
    return events;
  }
  const base::DictValue& choice = choices->front().GetDict();
  if (const base::DictValue* delta = choice.FindDict("delta")) {
    const std::string* reasoning = delta->FindString("reasoning_content");
    if (!reasoning) {
      reasoning = delta->FindString("reasoning");
    }
    if (reasoning && !reasoning->empty()) {
      base::DictValue thinking;
      thinking.Set("type", "thinking_delta");
      thinking.Set("thinking", *reasoning);
      Write("thinking", std::move(thinking), events);
    }
    const std::string* text = delta->FindString("content");
    if (text && !text->empty()) {
      base::DictValue text_delta;
      text_delta.Set("type", "text_delta");
      text_delta.Set("text", *text);
      Write("text", std::move(text_delta), events);
    }
    if (const base::ListValue* calls = delta->FindList("tool_calls")) {
      for (const base::Value& call : *calls) {
        if (call.is_dict()) {
          ToolDelta(call.GetDict(), events);
        }
      }
    }
  }
  const std::string* reason = choice.FindString("finish_reason");
  if (reason && !reason->empty()) {
    finish_reason_ = *reason;
  }
  return events;
}

std::vector<SseParser::Event> ChatStream::Finish() {
  if (ended_ || !finish_reason_) {
    return {};
  }
  return End();
}

void ChatStream::Write(std::string_view kind,
                       base::DictValue delta,
                       std::vector<SseParser::Event>& events) {
  if (!open_index_ || open_kind_ != kind) {
    CloseOpen(events);
    open_index_ = next_index_++;
    open_kind_ = std::string(kind);
    base::DictValue block;
    block.Set("type", kind);
    block.Set(kind, "");
    base::DictValue start;
    start.Set("index", *open_index_);
    start.Set("content_block", std::move(block));
    events.push_back(MakeEvent("content_block_start", std::move(start)));
  }
  base::DictValue data;
  data.Set("index", *open_index_);
  data.Set("delta", std::move(delta));
  events.push_back(MakeEvent("content_block_delta", std::move(data)));
}

void ChatStream::ToolDelta(const base::DictValue& call,
                           std::vector<SseParser::Event>& events) {
  // The call's index, else its id, else a new call per chunk (Gemini can
  // send whole calls without either).
  std::string key;
  const std::string* id = call.FindString("id");
  if (std::optional<int> index = call.FindInt("index")) {
    key = base::StrCat({"i", base::NumberToString(*index)});
  } else if (std::optional<double> index_double = call.FindDouble("index")) {
    key = base::StrCat(
        {"i", base::NumberToString(static_cast<int>(*index_double))});
  } else if (id && !id->empty()) {
    key = base::StrCat({"d", *id});
  } else {
    key = base::StrCat({"n", base::NumberToString(next_index_)});
  }
  const base::DictValue* function = call.FindDict("function");
  int index;
  auto it = tools_.find(key);
  if (it != tools_.end()) {
    index = it->second;
  } else {
    CloseOpen(events);
    index = next_index_++;
    tools_[key] = index;
    tool_order_.push_back(index);
    base::DictValue block;
    block.Set("type", "tool_use");
    block.Set("id", id && !id->empty()
                        ? *id
                        : base::StrCat({"call_", base::NumberToString(index)}));
    block.Set("name", function ? StringOr(*function, "name") : std::string());
    block.Set("input", base::DictValue());
    base::DictValue start;
    start.Set("index", index);
    start.Set("content_block", std::move(block));
    events.push_back(MakeEvent("content_block_start", std::move(start)));
  }
  std::string arguments;
  if (function) {
    if (const base::Value* value = function->Find("arguments")) {
      if (value->is_string()) {
        arguments = value->GetString();
      } else if (value->is_dict()) {
        // A server that sends the arguments as an object.
        arguments = Json(*value);
      }
    }
  }
  if (!arguments.empty()) {
    base::DictValue delta;
    delta.Set("type", "input_json_delta");
    delta.Set("partial_json", arguments);
    base::DictValue data;
    data.Set("index", index);
    data.Set("delta", std::move(delta));
    events.push_back(MakeEvent("content_block_delta", std::move(data)));
  }
}

void ChatStream::CloseOpen(std::vector<SseParser::Event>& events) {
  if (!open_index_) {
    return;
  }
  base::DictValue stop;
  stop.Set("index", *open_index_);
  events.push_back(MakeEvent("content_block_stop", std::move(stop)));
  open_index_.reset();
  open_kind_.clear();
}

std::vector<SseParser::Event> ChatStream::End() {
  ended_ = true;
  std::vector<SseParser::Event> events;
  CloseOpen(events);
  for (int index : tool_order_) {
    base::DictValue stop;
    stop.Set("index", index);
    events.push_back(MakeEvent("content_block_stop", std::move(stop)));
  }
  std::string reason;
  if (!tool_order_.empty()) {
    // Gemini says "stop" after tool calls too.
    reason = "tool_use";
  } else if (finish_reason_ == "length") {
    reason = "max_tokens";
  } else if (finish_reason_ == "content_filter") {
    reason = "refusal";
  } else {
    reason = "end_turn";
  }
  base::DictValue delta;
  delta.Set("stop_reason", reason);
  base::DictValue message_delta;
  message_delta.Set("delta", std::move(delta));
  events.push_back(MakeEvent("message_delta", std::move(message_delta)));
  events.push_back(MakeEvent("message_stop", base::DictValue()));
  return events;
}

// ResponsesStream

ResponsesStream::ResponsesStream() = default;
ResponsesStream::~ResponsesStream() = default;

int ResponsesStream::Open(const std::string& key,
                          base::DictValue block,
                          std::vector<SseParser::Event>& events) {
  auto it = blocks_.find(key);
  if (it != blocks_.end()) {
    return it->second;
  }
  const int index = next_index_++;
  blocks_[key] = index;
  order_.push_back(index);
  base::DictValue start;
  start.Set("index", index);
  start.Set("content_block", std::move(block));
  events.push_back(MakeEvent("content_block_start", std::move(start)));
  return index;
}

std::vector<SseParser::Event> ResponsesStream::Feed(std::string_view data) {
  std::vector<SseParser::Event> events;
  if (ended_) {
    return events;
  }
  std::optional<base::DictValue> event = base::JSONReader::ReadDict(
      base::TrimWhitespaceASCII(data, base::TRIM_ALL), base::JSON_PARSE_RFC);
  if (!event) {
    return events;
  }
  const int output = event->FindInt("output_index").value_or(0);
  const std::string output_id = base::NumberToString(output);
  const std::string type = StringOr(*event, "type");
  auto delta = [&](int block, base::DictValue value) {
    base::DictValue data;
    data.Set("index", block);
    data.Set("delta", std::move(value));
    events.push_back(MakeEvent("content_block_delta", std::move(data)));
  };
  auto call_block = [&](const base::DictValue& item) {
    base::DictValue block;
    block.Set("type", "tool_use");
    const std::string call_id = StringOr(item, "call_id");
    block.Set("id", call_id.empty() ? base::StrCat({"call_", output_id})
                                    : call_id);
    block.Set("name", StringOr(item, "name"));
    block.Set("input", base::DictValue());
    return block;
  };
  const base::DictValue* item = event->FindDict("item");
  const bool item_is_call =
      item && StringOr(*item, "type") == "function_call";
  if (type == "response.output_item.added") {
    if (item_is_call) {
      const int block =
          Open(base::StrCat({"call", output_id}), call_block(*item), events);
      calls_[output] = {block, false};
    }
  } else if (type == "response.output_text.delta" ||
             type == "response.refusal.delta") {
    const int block =
        Open(base::StrCat({"text", output_id}), TextBlock(""), events);
    base::DictValue value;
    value.Set("type", "text_delta");
    value.Set("text", StringOr(*event, "delta"));
    delta(block, std::move(value));
  } else if (type == "response.reasoning_summary_text.delta" ||
             type == "response.reasoning_text.delta") {
    base::DictValue thinking;
    thinking.Set("type", "thinking");
    thinking.Set("thinking", "");
    const int block = Open(base::StrCat({"thinking", output_id}),
                           std::move(thinking), events);
    base::DictValue value;
    value.Set("type", "thinking_delta");
    value.Set("thinking", StringOr(*event, "delta"));
    delta(block, std::move(value));
  } else if (type == "response.function_call_arguments.delta") {
    auto it = calls_.find(output);
    if (it != calls_.end()) {
      it->second.streamed = true;
      base::DictValue value;
      value.Set("type", "input_json_delta");
      value.Set("partial_json", StringOr(*event, "delta"));
      delta(it->second.block, std::move(value));
    }
  } else if (type == "response.output_item.done") {
    // A call whose arguments came whole.
    if (item_is_call) {
      if (!calls_.contains(output)) {
        const int block =
            Open(base::StrCat({"call", output_id}), call_block(*item), events);
        calls_[output] = {block, false};
      }
      Call& call = calls_.at(output);
      const std::string arguments = StringOr(*item, "arguments");
      if (!call.streamed && !arguments.empty()) {
        call.streamed = true;
        base::DictValue value;
        value.Set("type", "input_json_delta");
        value.Set("partial_json", arguments);
        delta(call.block, std::move(value));
      }
    }
  } else if (type == "response.completed") {
    return End("end_turn");
  } else if (type == "response.incomplete") {
    const std::string* reason =
        event->FindStringByDottedPath("response.incomplete_details.reason");
    return End(reason && *reason == "content_filter" ? "refusal"
                                                     : "max_tokens");
  } else if (type == "response.failed") {
    const std::string* message =
        event->FindStringByDottedPath("response.error.message");
    const std::string* code =
        event->FindStringByDottedPath("response.error.code");
    std::optional<std::string> plan =
        code ? chatgpt::PlanErrorMessage(*code) : std::nullopt;
    error_ = plan ? *plan : (message ? *message : "The reply failed.");
    ended_ = true;
  } else if (type == "error") {
    const std::string* message = event->FindString("message");
    if (!message) {
      message = event->FindStringByDottedPath("error.message");
    }
    error_ = message ? *message : "The reply failed.";
    ended_ = true;
  }
  return events;
}

std::vector<SseParser::Event> ResponsesStream::End(std::string_view reason) {
  ended_ = true;
  std::vector<SseParser::Event> events;
  for (int index : order_) {
    base::DictValue stop;
    stop.Set("index", index);
    events.push_back(MakeEvent("content_block_stop", std::move(stop)));
  }
  const std::string_view stop_reason =
      !calls_.empty() && reason == "end_turn" ? "tool_use" : reason;
  base::DictValue delta;
  delta.Set("stop_reason", stop_reason);
  base::DictValue message_delta;
  message_delta.Set("delta", std::move(delta));
  events.push_back(MakeEvent("message_delta", std::move(message_delta)));
  events.push_back(MakeEvent("message_stop", base::DictValue()));
  return events;
}

// StreamTranslator

StreamTranslator::StreamTranslator(AIProvider provider)
    : responses_(UsesResponsesApi(provider)) {}

StreamTranslator::~StreamTranslator() = default;

std::vector<SseParser::Event> StreamTranslator::Feed(std::string_view data) {
  return responses_ ? responses_stream_.Feed(data) : chat_.Feed(data);
}

std::vector<SseParser::Event> StreamTranslator::Finish() {
  // A Responses stream without response.completed ended early.
  return responses_ ? std::vector<SseParser::Event>() : chat_.Finish();
}

const std::optional<std::string>& StreamTranslator::error() const {
  return responses_ ? responses_stream_.error() : chat_.error();
}

}  // namespace compass::claude::openai
