// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/claude_bridge.h"

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
    "question, between <page> tags (it may be cut short). Answer briefly, "
    "in the user's language, and say so when the page doesn't tell.";

}  // namespace

ClaudeBridge::ClaudeBridge(JNIEnv* env,
                           const base::android::JavaRef<jobject>& java_object,
                           Profile* profile)
    : java_object_(env, java_object), profile_(profile) {}

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
    OnDone("Add your Anthropic API key first.");
    return;
  }
  claude::Endpoint endpoint = claude::EndpointFromPrefs(*profile_->GetPrefs());
  endpoint.api_key = *key;
  client_ = std::make_unique<claude::ClaudeClient>(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      std::move(endpoint));

  base::ListValue messages;
  // Earlier turns as they were, the last question with its page.
  for (size_t i = 0; i + 1 < messages_.size(); ++i) {
    if (messages_[i].text.empty()) {
      continue;
    }
    base::DictValue turn;
    turn.Set("role", messages_[i].role);
    turn.Set("content", messages_[i].text);
    messages.Append(std::move(turn));
  }
  base::DictValue question;
  question.Set("role", "user");
  question.Set("content", content);
  messages.Append(std::move(question));
  std::string model = profile_->GetPrefs()->GetString(claude::prefs::kModel);
  if (model.empty()) {
    model = claude::kDefaultModel;
  }
  base::DictValue request;
  request.Set("model", model);
  request.Set("max_tokens", 4096);
  request.Set("system", kSystemPrompt);
  request.Set("messages", std::move(messages));
  messages_.push_back({"assistant", std::string()});
  client_->StreamMessage(
      std::move(request), {},
      base::BindRepeating(&ClaudeBridge::OnEvent, weak_factory_.GetWeakPtr()),
      base::BindOnce(&ClaudeBridge::OnDone, weak_factory_.GetWeakPtr()));
}

void ClaudeBridge::OnEvent(const claude::SseParser::Event& event) {
  if (event.name != "content_block_delta") {
    return;
  }
  std::optional<base::Value> data = base::JSONReader::Read(event.data, base::JSON_PARSE_RFC);
  const base::DictValue* dict = data ? data->GetIfDict() : nullptr;
  const base::DictValue* delta = dict ? dict->FindDict("delta") : nullptr;
  const std::string* text = delta ? delta->FindString("text") : nullptr;
  if (text && !messages_.empty() && messages_.back().role == "assistant") {
    messages_.back().text += *text;
    Changed();
  }
}

void ClaudeBridge::OnDone(std::optional<std::string> error) {
  streaming_ = false;
  if (error) {
    error_ = *error;
    // A reply that never came leaves no empty turn.
    if (!messages_.empty() && messages_.back().role == "assistant" &&
        messages_.back().text.empty()) {
      messages_.pop_back();
    }
  }
  Changed();
}

void ClaudeBridge::Stop(JNIEnv* env) {
  if (client_) {
    client_->Cancel();
  }
  OnDone(std::nullopt);
}

void ClaudeBridge::Clear(JNIEnv* env) {
  if (client_) {
    client_->Cancel();
  }
  messages_.clear();
  streaming_ = false;
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
