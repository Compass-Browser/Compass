// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_

#include <jni.h>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/browser/claude/ai_provider.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_memory.h"
#include "compass/browser/claude/claude_tools.h"
#include "compass/browser/claude/page_reader.h"
#include "compass/browser/android/claude_tabs_android.h"
#include "compass/browser/boosts/boost_store.h"
#include "compass/browser/claude/sse_parser.h"
#include "components/prefs/pref_change_registrar.h"
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
class ClaudeBridge : public claude::ClaudeMemory::Observer,
                     public boosts::BoostStore::Observer,
                     public ClaudeTabsAndroid::Window {
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
  // key ("has_key", "key_hint") and whether the chat can ask ("set_up"),
  // the provider (docs/design/ai-providers.md: "provider", the "providers"
  // to pick from, id and name, its "key_placeholder", "default_base_url", "privacy_note" and
  // "known_models", id and name; none for Anthropic, whose list the sheet
  // keeps), whether Claude may use the whole browser, the model in use, and
  // the memory: "memory_enabled", "memory_save" and the "memories" (id,
  // text; oldest first), and Claude in the sidebar's "features" (key, on,
  // model; empty is the chat's), as JSON. The key, server and models are
  // the provider's.
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
  // The model the next reply uses (the desktop's compass.claude.model, or
  // the provider's variant; empty: the default).
  void SetModel(JNIEnv* env, const std::string& model);
  // Who answers: a claude::ProviderId() ("anthropic", "openai", "gemini",
  // "compatible"). Each provider keeps its own key, server and models.
  void SetProvider(JNIEnv* env, const std::string& provider);

  // Claude in the sidebar (docs/design/ai-sidebar.md): a feature's switch and
  // its model, by the feature's key ("name_link_folders", "tidy_today",
  // "short_tab_titles", "command_bar_ask"; the snapshot's "features").
  void SetFeature(JNIEnv* env, const std::string& feature, bool on);
  void SetFeatureModel(JNIEnv* env,
                       const std::string& feature,
                       const std::string& model);
  // Continue in Chat (Ask in the command bar): a new chat whose first
  // exchange is |question| (as shown; |sent| as Claude got it, with the page
  // line) and |answer|. Nothing is sent until the user writes.
  void Seed(JNIEnv* env,
            const std::string& question,
            const std::string& sent,
            const std::string& answer);

  // Memory (docs/design/claude.md, "Memory"): the settings, Undo of a
  // change Claude made, and the user's own changes to the list.
  void SetMemoryEnabled(JNIEnv* env, bool enabled);
  void SetMemorySave(JNIEnv* env, bool enabled);
  void UndoMemory(JNIEnv* env, int32_t undo_id);
  void AddMemory(JNIEnv* env, const std::string& text);
  // The user's edit of memory |id|.
  void UpdateMemory(JNIEnv* env, const std::string& id, const std::string& text);
  void RemoveMemory(JNIEnv* env, const std::string& id);
  void RemoveAllMemories(JNIEnv* env);

  // The endpoint (docs/design/claude.md, "Settings"), the provider's: a base
  // URL (empty for its own server; for a compatible server, none) and, for
  // Anthropic, how the key is sent (claude::AuthStyle). False, with the
  // error in the snapshot, for a URL ParseBaseUrl() refuses.
  bool SetEndpoint(JNIEnv* env, const std::string& base_url, int32_t auth_style);
  // Test Connection: the key and endpoint, through the model list (the
  // snapshot's "testing", "test_ok", "test_result" and "api_models").
  void TestConnection(JNIEnv* env);
  // The page attachment: whether questions carry the page's text (each page
  // once a chat, as on the desktop and iOS).
  void SetAttachPage(JNIEnv* env, bool attach);
  // Asks the last question again after a failed reply.
  void Retry(JNIEnv* env);
  // A tool row's screenshot (a "tool" message with "image"), base64 JPEG;
  // empty if it's gone.
  std::string ToolImage(JNIEnv* env, const std::string& tool_use_id);

  // "Accept the risks of subscription tokens" (docs/design/claude.md,
  // "Settings"): a token from `claude setup-token` may be the key.
  void SetAcceptTokenRisks(JNIEnv* env, bool accepted);
  // Developer tools (docs/design/claude.md, "Developer tools"): "Let Claude
  // use developer tools".
  void SetDevTools(JNIEnv* env, bool allowed);
  // Files (docs/design/claude.md, "Files"): 0 off, 1 save files (into
  // Downloads), 2 use a folder: |folder|, a folder of Downloads by name
  // (made if it isn't there).
  void SetFiles(JNIEnv* env, int32_t access, const std::string& folder);
  // The user's answer to approval |id| (the snapshot's "approval"): 0 deny,
  // 1 allow, 2 allow that tool for the rest of the chat.
  void Answer(JNIEnv* env, int32_t id, int32_t decision);

  // Boosts (docs/design/boosts.md): "Let Claude make Boosts", Undo of a
  // change Claude made (a "boost" message's undo id), and the user's own
  // changes to the list (the snapshot's "boosts").
  void SetBoostsAllowed(JNIEnv* env, bool allowed);
  void UndoBoost(JNIEnv* env, int32_t undo_id);
  void SetBoostEnabled(JNIEnv* env, const std::string& id, bool enabled);
  void RemoveBoost(JNIEnv* env, const std::string& id);

  // claude::ClaudeMemory::Observer:
  void OnMemoriesChanged() override;

  // boosts::BoostStore::Observer:
  void OnBoostsChanged() override;

  // ClaudeTabsAndroid::Window, in Java (ClaudeChat):
  std::string ShownSpace() override;
  void ShowNode(const std::string& node_id,
                const std::string& url,
                const std::string& space_id) override;
  void LoadInBackground(const std::string& node_id,
                        const std::string& url) override;
  void SwitchSpace(const std::string& space_id) override;

 private:
  struct Message {
    std::string role;
    // "thinking": the summary so far; "tool": the tool's name.
    std::string text;
    // A "tool" message: its tool_use id, its input (JSON), its result's text,
    // whether that's an error, and whether it ran. A "thinking" one: done
    // once its block ended.
    std::string tool_use_id;
    // A "user" message: the title of the page it carried, if one.
    std::string page;
    // A "tool" message: it answered with a screenshot (ToolImage()).
    bool image = false;
    std::string input;
    std::string result;
    bool error = false;
    bool done = false;
    // A "memory" message's change, and its Undo.
    std::string kind;
    int undo_id = 0;
    bool undone = false;
  };
  // A content block of the reply that streams.
  struct Block {
    std::string type;
    // A text block's text, or a thinking block's summary.
    std::string text;
    // A thinking block's signature; a redacted_thinking block's data. They go
    // back unchanged in the next request, as the API wants during tool use.
    std::string signature;
    std::string data;
    // The message showing it (thinking, tool), or -1.
    int message = -1;
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
  // The file tools' setting now (ClaudeTools::FilesConfigGetter).
  claude::FilesConfig GetFilesConfig();
  // ClaudeTools's approver: asks the user in the sheet, unless they allowed
  // the tool for this chat.
  void Ask(claude::ApprovalRequest request,
           base::OnceCallback<void(bool)> done);
  // Denies what waits for the user (Stop, a new chat).
  void DenyAll();

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
  // The cache breakpoints to send: lower when the endpoint (a gateway adding
  // its own) refused them (claude::CacheLevelFor()). |stepped_down_|: the
  // level that gets through is remembered.
  GURL base_url_;
  // Who answers the chat's requests (the endpoint's when the question went).
  claude::AIProvider provider_ = claude::AIProvider::kAnthropic;
  claude::CacheLevel cache_level_ = claude::CacheLevel::kNormal;
  bool stepped_down_ = false;
  // The key is a subscription token the user accepted the risks of: it goes
  // as Claude Code (claude::UsesOAuthToken()).
  bool oauth_token_ = false;
  // Each memory change's state before it, for Undo.
  std::map<int, claude::MemoryChange> memory_undo_;
  // Each Boost change's state before it, for Undo.
  std::map<int, claude::BoostChange> boost_undo_;
  int next_undo_id_ = 1;
  // Changes waiting for the user's approval, first shown first.
  struct Approval {
    Approval();
    Approval(Approval&&);
    Approval& operator=(Approval&&);
    ~Approval();
    int id = 0;
    claude::ApprovalRequest request;
    base::OnceCallback<void(bool)> done;
  };
  std::deque<Approval> approvals_;
  int next_approval_id_ = 1;
  // Tools the user allowed for this chat.
  std::set<std::string> allowed_tools_;
  // The developer tools went with a request of this chat: they stay for the
  // rest of it, so its tools only grow (docs/design/claude.md).
  bool chat_devtools_ = false;
  // The same for the Boost tools.
  bool chat_boosts_ = false;
  // The page attachment, and the page the chat has (each goes once).
  bool attach_page_ = true;
  std::string attached_url_;
  // The question's turn as it was sent, for Retry.
  std::string retry_content_;
  // The user message the page goes with (its index), while it's read.
  size_t asking_message_ = 0;
  // Test Connection.
  std::unique_ptr<claude::ClaudeClient> test_client_;
  bool testing_ = false;
  bool test_ok_ = false;
  std::string test_result_;
  std::vector<claude::ModelInfo> api_models_;
  // The latest screenshots, by tool_use id.
  std::map<std::string, std::string> images_;
  std::vector<std::string> image_order_;
  // The provider and its key, base URL and chat model.
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<claude::ClaudeMemory, claude::ClaudeMemory::Observer>
      memory_observation_{this};
  base::ScopedObservation<boosts::BoostStore, boosts::BoostStore::Observer>
      boosts_observation_{this};
  base::WeakPtrFactory<ClaudeBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_CLAUDE_BRIDGE_H_
