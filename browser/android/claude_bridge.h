// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_

#include <jni.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/browser/claude/claude_memory.h"
#include "compass/browser/claude/claude_tools.h"
#include "compass/browser/claude/page_reader.h"
#include "compass/browser/claude/sse_parser.h"
#include "url/gurl.h"

class Profile;

namespace content {
class WebContents;
}

namespace compass {

namespace claude {
class ClaudeClient;
class ClaudeTools;
struct ToolResult;
}  // namespace claude

// The native half of it.cipi1965.compass.claude.ClaudeBridge: a chat with
// Claude about the page on screen (docs/design/android.md, C8), on the
// desktop's client (ClaudeClient), key storage (ClaudeCredentials), page
// reader and tools (ClaudeTools). Each question carries the page's text;
// Claude can act on the page with the page tools and, with "Let Claude use
// the whole browser" on, on the tabs (ClaudeTabsAndroid). A reply that asks
// for tools gets their results and goes on, as the desktop's chat does.
class ClaudeBridge : public claude::ClaudeMemory::Observer {
 public:
  ClaudeBridge(JNIEnv* env,
               const base::android::JavaRef<jobject>& java_object,
               Profile* profile);
  ClaudeBridge(const ClaudeBridge&) = delete;
  ClaudeBridge& operator=(const ClaudeBridge&) = delete;
  ~ClaudeBridge() override;

  void Destroy(JNIEnv* env);
  // The conversation (roles "user", "assistant", "tool", for a tool Claude
  // used, and "memory", for a change to its memory: "kind" "saved",
  // "updated" or "forgot", the text, its "undo" id and whether it was
  // "undone"), whether a reply streams, the last error, whether there's a
  // key, whether Claude may use the whole browser, the model in use, and
  // the memory: "memory_enabled", "memory_save" and the "memories" (id,
  // text; oldest first), as JSON.
  std::string Snapshot(JNIEnv* env);
  void SetApiKey(JNIEnv* env, const std::string& key);
  // "Let Claude use the whole browser" (the desktop's setting).
  void SetBrowserScope(JNIEnv* env, bool allowed);
  // Asks |text| about the page |contents| shows (null: no page); the page
  // tools act on that tab.
  void Send(JNIEnv* env,
            const std::string& text,
            content::WebContents* contents);
  void Stop(JNIEnv* env);
  void Clear(JNIEnv* env);
  // The model the next reply uses (the desktop's compass.claude.model; empty:
  // the default).
  void SetModel(JNIEnv* env, const std::string& model);

  // Memory (docs/design/claude.md, "Memory"): the settings, Undo of a
  // change Claude made, and the user's own changes to the list.
  void SetMemoryEnabled(JNIEnv* env, bool enabled);
  void SetMemorySave(JNIEnv* env, bool enabled);
  void UndoMemory(JNIEnv* env, int32_t undo_id);
  void AddMemory(JNIEnv* env, const std::string& text);
  void RemoveMemory(JNIEnv* env, const std::string& id);
  void RemoveAllMemories(JNIEnv* env);

  // claude::ClaudeMemory::Observer:
  void OnMemoriesChanged() override;

 private:
  struct Message {
    std::string role;
    std::string text;
    // A "memory" message's change, and its Undo.
    std::string kind;
    int undo_id = 0;
    bool undone = false;
  };
  // A content block of the reply that streams.
  struct Block {
    std::string type;
    std::string text;
    std::string id;
    std::string name;
    // A tool_use's input, as it streams in.
    std::string json;
  };

  void OnPage(std::string question,
              base::expected<claude::PageText, std::string> page);
  void OnKey(std::string content, std::optional<std::string> key);
  // Streams the next reply to |conversation_|.
  void Request();
  void OnEvent(const claude::SseParser::Event& event);
  void OnDone(std::optional<std::string> error);
  // Runs the reply's tool_use blocks from |index| on, one at a time, then
  // asks for the next reply with their results.
  void RunTools(size_t index);
  void OnToolDone(size_t index,
                  std::string tool_use_id,
                  claude::ToolResult result);
  void Finish(std::optional<std::string> error);
  // Drops a last turn the API couldn't take back: a question without its
  // reply, or tool uses without their results (after Stop()).
  void TrimConversation();
  void Changed();

  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  raw_ptr<Profile> profile_;
  // What the chat shows.
  std::vector<Message> messages_;
  // What the API gets: the Messages API's "messages", with content blocks.
  base::ListValue conversation_;
  std::vector<Block> blocks_;
  std::string stop_reason_;
  base::ListValue tool_results_;
  // Tool rounds for the current question.
  int rounds_ = 0;
  std::unique_ptr<claude::ClaudeClient> client_;
  std::unique_ptr<claude::ClaudeTools> tools_;
  bool streaming_ = false;
  std::string error_;
  // The memory as the chat's first question took it: the <memory> block
  // (empty: none) and whether the memory tools go with each request. The
  // system prompt stays the same within a chat, as on the desktop.
  bool memory_frozen_ = false;
  std::string memory_block_;
  bool memory_tools_ = false;
  // Only the top-level cache breakpoint: the endpoint (a gateway adding its
  // own) refused the usual ones (claude::UsesMinimalCaching()). |retrying_|:
  // asking again after that refusal; its success is remembered.
  GURL base_url_;
  bool minimal_caching_ = false;
  bool retrying_ = false;
  // Each memory change's state before it, for Undo.
  std::map<int, claude::MemoryChange> memory_undo_;
  int next_undo_id_ = 1;
  base::ScopedObservation<claude::ClaudeMemory, claude::ClaudeMemory::Observer>
      memory_observation_{this};
  base::WeakPtrFactory<ClaudeBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_
