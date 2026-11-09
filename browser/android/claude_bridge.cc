// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/claude_bridge.h"

#include <algorithm>
#include <utility>

#include "base/android/jni_string.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_client.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_tools.h"
#include "compass/browser/android/claude_tabs_android.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

// Must come after the class declaration.
#include "compass/android/jni_headers/ClaudeBridge_jni.h"

namespace compass {

namespace {

// A page's text, at most (the desktop's page tools read the same way).
constexpr int kMaxPageChars = 60000;

constexpr char kSystemPrompt[] =
    "You are Claude, in the Compass web browser on an Android phone. The "
    "user asks about the page on screen, whose text comes with each "
    "question, between <page> tags (it may be cut short). You can also act "
    "on that page with the page tools (read it, find, click, type, scroll, "
    "navigate, screenshot) and, when the user lets you use the whole "
    "browser, on the tabs (list, open, select, close). Answer briefly, in "
    "the user's language, and say so when the page doesn't tell.";

// Tool rounds for one question, at most.
constexpr int kMaxToolRounds = 20;

}  // namespace

ClaudeBridge::ClaudeBridge(JNIEnv* env,
                           const base::android::JavaRef<jobject>& java_object,
                           Profile* profile)
    : java_object_(env, java_object),
      profile_(profile),
      tools_(std::make_unique<claude::ClaudeTools>(
          std::make_unique<ClaudeTabsAndroid>(profile))) {}

ClaudeBridge::~ClaudeBridge() = default;

void ClaudeBridge::Destroy(JNIEnv* env) {
  delete this;
}

std::string ClaudeBridge::Snapshot(JNIEnv* env) {
  base::DictValue snapshot;
  base::ListValue messages;
  for (const Message& message : messages_) {
    base::DictValue dict;
    dict.Set("role", message.role);
    dict.Set("text", message.text);
    messages.Append(std::move(dict));
  }
  snapshot.Set("messages", std::move(messages));
  snapshot.Set("streaming", streaming_);
  snapshot.Set("error", error_);
  snapshot.Set("has_key", claude::ClaudeCredentials::HasApiKey(profile_));
  snapshot.Set("browser_scope",
               profile_->GetPrefs()->GetBoolean(claude::prefs::kBrowserScope));
  return base::WriteJson(snapshot).value_or("{}");
}

void ClaudeBridge::SetApiKey(JNIEnv* env, const std::string& key) {
  claude::ClaudeCredentials::SetApiKey(
      profile_, key,
      base::BindOnce(
          [](base::WeakPtr<ClaudeBridge> self, bool saved) {
            if (!self) {
              return;
            }
            self->error_ = saved ? std::string() : "Couldn't save the key.";
            self->Changed();
          },
          weak_factory_.GetWeakPtr()));
}

void ClaudeBridge::SetBrowserScope(JNIEnv* env, bool allowed) {
  profile_->GetPrefs()->SetBoolean(claude::prefs::kBrowserScope, allowed);
  Changed();
}

void ClaudeBridge::Send(JNIEnv* env,
                        const std::string& text,
                        content::WebContents* contents) {
  if (streaming_ || text.empty()) {
    return;
  }
  error_.clear();
  messages_.push_back({"user", text});
  streaming_ = true;
  Changed();
  if (contents) {
    // The page tools act on the tab the question is about.
    tools_->SetTarget(contents);
  }
  if (contents &&
      claude::CanUsePage(contents->GetLastCommittedURL())) {
    claude::ReadPageText(contents, kMaxPageChars,
                         base::BindOnce(&ClaudeBridge::OnPage,
                                        weak_factory_.GetWeakPtr(), text));
  } else {
    OnPage(text, base::unexpected(std::string()));
  }
}

void ClaudeBridge::OnPage(std::string question,
                          base::expected<claude::PageText, std::string> page) {
  std::string content = question;
  if (page.has_value()) {
    content = "<page url=\"" + page->url + "\" title=\"" + page->title +
              "\">\n" + page->text + "\n</page>\n\n" + question;
  }
  claude::ClaudeCredentials::GetApiKey(
      profile_, base::BindOnce(&ClaudeBridge::OnKey,
                               weak_factory_.GetWeakPtr(), content));
}

void ClaudeBridge::OnKey(std::string content, std::optional<std::string> key) {
  if (!key || key->empty()) {
    Finish("Add your Anthropic API key first.");
    return;
  }
  claude::Endpoint endpoint = claude::EndpointFromPrefs(*profile_->GetPrefs());
  endpoint.api_key = *key;
  client_ = std::make_unique<claude::ClaudeClient>(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      std::move(endpoint));
  conversation_.Append(
      base::DictValue().Set("role", "user").Set("content", content));
  rounds_ = 0;
  Request();
}

void ClaudeBridge::Request() {
  std::string model = profile_->GetPrefs()->GetString(claude::prefs::kModel);
  if (model.empty()) {
    model = claude::kDefaultModel;
  }
  base::DictValue request;
  request.Set("model", model);
  request.Set("max_tokens", 4096);
  request.Set("system", kSystemPrompt);
  request.Set("tools", claude::ClaudeTools::Definitions());
  request.Set("messages", conversation_.Clone());
  blocks_.clear();
  stop_reason_.clear();
  client_->StreamMessage(
      std::move(request), {},
      base::BindRepeating(&ClaudeBridge::OnEvent, weak_factory_.GetWeakPtr()),
      base::BindOnce(&ClaudeBridge::OnDone, weak_factory_.GetWeakPtr()));
}

void ClaudeBridge::OnEvent(const claude::SseParser::Event& event) {
  std::optional<base::Value> data =
      base::JSONReader::Read(event.data, base::JSON_PARSE_RFC);
  const base::DictValue* dict = data ? data->GetIfDict() : nullptr;
  if (!dict) {
    return;
  }
  const size_t index =
      static_cast<size_t>(std::max(0, dict->FindInt("index").value_or(0)));
  if (event.name == "content_block_start") {
    const base::DictValue* start = dict->FindDict("content_block");
    if (!start) {
      return;
    }
    if (blocks_.size() <= index) {
      blocks_.resize(index + 1);
    }
    Block& block = blocks_[index];
    const std::string* type = start->FindString("type");
    block.type = type ? *type : std::string();
    if (const std::string* id = start->FindString("id")) {
      block.id = *id;
    }
    if (const std::string* name = start->FindString("name")) {
      block.name = *name;
    }
    if (block.type == "tool_use") {
      messages_.push_back({"tool", block.name});
      Changed();
    }
    return;
  }
  if (event.name == "content_block_delta") {
    const base::DictValue* delta = dict->FindDict("delta");
    if (!delta || index >= blocks_.size()) {
      return;
    }
    Block& block = blocks_[index];
    if (const std::string* json = delta->FindString("partial_json")) {
      block.json += *json;
      return;
    }
    const std::string* text = delta->FindString("text");
    if (!text) {
      return;
    }
    block.text += *text;
    if (messages_.empty() || messages_.back().role != "assistant") {
      messages_.push_back({"assistant", std::string()});
    }
    messages_.back().text += *text;
    Changed();
    return;
  }
  if (event.name == "message_delta") {
    const std::string* reason = dict->FindStringByDottedPath("delta.stop_reason");
    if (reason) {
      stop_reason_ = *reason;
    }
  }
}

void ClaudeBridge::OnDone(std::optional<std::string> error) {
  if (error) {
    Finish(error);
    return;
  }
  base::ListValue content;
  bool uses_tools = false;
  for (const Block& block : blocks_) {
    if (block.type == "text" && !block.text.empty()) {
      content.Append(
          base::DictValue().Set("type", "text").Set("text", block.text));
    } else if (block.type == "tool_use") {
      std::optional<base::Value> input =
          base::JSONReader::Read(block.json.empty() ? "{}" : block.json,
                                 base::JSON_PARSE_RFC);
      content.Append(base::DictValue()
                         .Set("type", "tool_use")
                         .Set("id", block.id)
                         .Set("name", block.name)
                         .Set("input", input && input->is_dict()
                                           ? std::move(*input)
                                           : base::Value(base::DictValue())));
      uses_tools = true;
    }
  }
  if (content.empty()) {
    Finish(std::nullopt);
    return;
  }
  conversation_.Append(base::DictValue()
                           .Set("role", "assistant")
                           .Set("content", std::move(content)));
  if (!uses_tools || stop_reason_ != "tool_use") {
    Finish(std::nullopt);
    return;
  }
  if (++rounds_ > kMaxToolRounds) {
    TrimConversation();
    Finish("Claude used tools for too long; ask again to go on.");
    return;
  }
  tool_results_ = base::ListValue();
  RunTools(0);
}

void ClaudeBridge::RunTools(size_t index) {
  while (index < blocks_.size() && blocks_[index].type != "tool_use") {
    ++index;
  }
  if (index >= blocks_.size()) {
    // Every tool ran: the results go back as the next user turn.
    conversation_.Append(base::DictValue()
                             .Set("role", "user")
                             .Set("content", std::move(tool_results_)));
    tool_results_ = base::ListValue();
    Request();
    return;
  }
  const Block& block = blocks_[index];
  std::optional<base::Value> input = base::JSONReader::Read(
      block.json.empty() ? "{}" : block.json, base::JSON_PARSE_RFC);
  tools_->Run(block.name,
              input && input->is_dict() ? std::move(*input).TakeDict()
                                        : base::DictValue(),
              base::BindOnce(&ClaudeBridge::OnToolDone,
                             weak_factory_.GetWeakPtr(), index, block.id));
}

void ClaudeBridge::OnToolDone(size_t index,
                              std::string tool_use_id,
                              claude::ToolResult result) {
  if (!streaming_) {
    return;
  }
  base::DictValue dict = std::move(result).ToDict();
  dict.Set("type", "tool_result");
  dict.Set("tool_use_id", tool_use_id);
  tool_results_.Append(std::move(dict));
  RunTools(index + 1);
}

void ClaudeBridge::Finish(std::optional<std::string> error) {
  streaming_ = false;
  if (error) {
    error_ = *error;
    TrimConversation();
  }
  // A reply that never came leaves no empty turn.
  if (!messages_.empty() && messages_.back().role == "assistant" &&
      messages_.back().text.empty()) {
    messages_.pop_back();
  }
  Changed();
}

void ClaudeBridge::TrimConversation() {
  while (!conversation_.empty()) {
    const base::DictValue& last = conversation_.back().GetDict();
    const std::string* role = last.FindString("role");
    bool asks_for_tools = false;
    if (const base::ListValue* content = last.FindList("content")) {
      for (const base::Value& block : *content) {
        const std::string* type = block.GetDict().FindString("type");
        asks_for_tools |= type && *type == "tool_use";
      }
    }
    if (role && *role == "assistant" && !asks_for_tools) {
      return;
    }
    conversation_.erase(conversation_.end() - 1);
  }
}

void ClaudeBridge::Stop(JNIEnv* env) {
  // First: stopping a tool runs its callback, which goes on only while a
  // reply is under way.
  streaming_ = false;
  if (client_) {
    client_->Cancel();
  }
  tools_->Stop();
  Finish(std::nullopt);
  TrimConversation();
}

void ClaudeBridge::Clear(JNIEnv* env) {
  streaming_ = false;
  if (client_) {
    client_->Cancel();
  }
  tools_->Stop();
  messages_.clear();
  conversation_ = base::ListValue();
  error_.clear();
  Changed();
}

void ClaudeBridge::Changed() {
  Java_ClaudeBridge_onChanged(jni_zero::AttachCurrentThread(), java_object_);
}

static int64_t JNI_ClaudeBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(new ClaudeBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(ClaudeBridge)
