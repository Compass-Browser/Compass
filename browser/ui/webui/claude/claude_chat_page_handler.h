// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_PAGE_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_PAGE_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/files/file_path.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/browser/claude/page_reader.h"
#include "compass/browser/claude/sse_parser.h"
#include "compass/browser/ui/webui/claude/chat.mojom.h"
#include "components/prefs/pref_change_registrar.h"
#include "compass/browser/claude/claude_tools.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

class BrowserWindowInterface;
class TopChromeWebUIController;

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace compass::claude {

class ClaudeClient;
class ClaudeCodeProcess;

// The browser side of the chat panel: tells the page about the settings and
// the active tab, reads the tab's text, and streams Claude's replies with the
// profile's key, which the page never sees. The page keeps the conversation.
// See docs/design/claude.md, "Chat panel".
class ClaudeChatPageHandler : public chat::mojom::PageHandler,
                              public content::WebContentsObserver {
 public:
  // |controller| shows and closes the page's view; null in tests.
  ClaudeChatPageHandler(
      mojo::PendingReceiver<chat::mojom::PageHandler> receiver,
      mojo::PendingRemote<chat::mojom::Page> page,
      BrowserWindowInterface* browser,
      TopChromeWebUIController* controller);
  ClaudeChatPageHandler(const ClaudeChatPageHandler&) = delete;
  ClaudeChatPageHandler& operator=(const ClaudeChatPageHandler&) = delete;
  ~ClaudeChatPageHandler() override;

  // Where requests go; the profile's network by default.
  void SetLoaderFactoryForTesting(
      scoped_refptr<network::SharedURLLoaderFactory> factory);

  // chat::mojom::PageHandler:
  void ShowUI() override;
  void GetSetup(GetSetupCallback callback) override;
  void GetActivePage(GetActivePageCallback callback) override;
  void ReadActivePage(ReadActivePageCallback callback) override;
  void Send(const std::string& messages_json) override;
  void GetContext(GetContextCallback callback) override;
  void StartTask() override;
  void RunTool(const std::string& name,
               const std::string& input_json,
               RunToolCallback callback) override;
  void Stop() override;
  void OpenSettings() override;
  void SetModel(const std::string& model) override;
  void OpenLink(const std::string& url) override;
  void ClosePanel() override;
  void TakeQuestion(TakeQuestionCallback callback) override;
  void UndoMemory(int32_t undo_id, UndoMemoryCallback callback) override;

  // The command bar's "Ask Claude", while the page is connected.
  void Ask(const std::string& question);

  // content::WebContentsObserver (the active tab):
  void PrimaryPageChanged(content::Page& page) override;
  void TitleWasSet(content::NavigationEntry* entry) override;

 private:
  chat::mojom::SetupPtr MakeSetup();
  // The window's tabs, titles and URLs, for the request's context.
  std::string WindowTabs();
  chat::mojom::ActivePagePtr MakeActivePage();
  void OnSettingsChanged();
  void OnActiveTabChanged(BrowserWindowInterface* browser);
  void OnPageRead(ReadActivePageCallback callback,
                  base::expected<PageText, std::string> page);
  void OnKeyForSend(int request_id,
                    std::string messages_json,
                    std::optional<std::string> key);
  void OnStreamEvent(const SseParser::Event& event);
  void OnStreamDone(int request_id, std::optional<std::string> error);
  // The memory a chat gets, taken as it starts (|new_chat|) and kept for
  // the rest of it: it's in the system prompt (docs/design/claude.md,
  // "Memory").
  struct ChatMemory {
    std::string block;
    bool tools = false;
  };
  const ChatMemory& FreezeMemory(bool new_chat);
  // A tool's result as RunTool() gives it: with a memory change's undo id.
  std::string ToolResultJson(ToolResult result);

  // Claude Code mode (docs/design/claude.md, D8).
  bool UsesClaudeCode() const;
  void SendToClaudeCode(base::ListValue messages);
  // Where `claude` is (FindClaudeCode(), off this thread), and the folder it
  // runs in; then the message goes.
  void OnClaudeCodeFound(int request_id,
                         std::string line,
                         std::optional<base::FilePath> program);
  void OnClaudeCodeLine(std::string line);
  void OnClaudeCodeExit(int exit_code);
  // Ends the process; the conversation goes on with --resume, unless
  // |forget| (a new chat).
  void EndClaudeCode(bool forget);
  // Claude Code's calls of Compass's tools, through compass-mcp --chat.
  void RunChatTool(const std::string& name,
                   base::DictValue arguments,
                   base::OnceCallback<void(ToolResult)> done);

  mojo::Receiver<chat::mojom::PageHandler> receiver_;
  mojo::Remote<chat::mojom::Page> page_;
  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<TopChromeWebUIController> controller_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  std::unique_ptr<ClaudeClient> client_;
  ClaudeTools tools_;
  std::unique_ptr<ClaudeCodeProcess> claude_code_;
  // Claude Code's session, for --resume after Stop or another model.
  std::string claude_code_session_;
  // The model the process runs.
  std::string claude_code_model_;
  // A reply is under way: the process ending is an error.
  bool claude_code_replying_ = false;
  // The last of its output that wasn't JSON (stderr), to say why it ended.
  std::string claude_code_output_;
  // McpBridge::RegisterChat()'s token, while registered.
  std::string chat_token_;
  std::optional<ChatMemory> chat_memory_;
  // What the memory tools changed in this page's chats, by undo id.
  std::map<int32_t, MemoryChange> memory_undo_;
  int32_t next_undo_id_ = 1;
  // Counts Send() calls, so a stopped request's late callbacks are ignored.
  int request_id_ = 0;
  PrefChangeRegistrar pref_registrar_;
  base::CallbackListSubscription active_tab_subscription_;
  base::WeakPtrFactory<ClaudeChatPageHandler> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_PAGE_HANDLER_H_
