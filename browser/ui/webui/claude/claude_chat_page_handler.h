// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_PAGE_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_PAGE_HANDLER_H_

#include <memory>
#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "compass/browser/claude/page_reader.h"
#include "compass/browser/claude/sse_parser.h"
#include "compass/browser/ui/webui/claude/chat.mojom.h"
#include "components/prefs/pref_change_registrar.h"
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
  void Stop() override;
  void OpenSettings() override;
  void OpenLink(const std::string& url) override;
  void ClosePanel() override;
  void TakeQuestion(TakeQuestionCallback callback) override;

  // The command bar's "Ask Claude", while the page is connected.
  void Ask(const std::string& question);

  // content::WebContentsObserver (the active tab):
  void PrimaryPageChanged(content::Page& page) override;
  void TitleWasSet(content::NavigationEntry* entry) override;

 private:
  chat::mojom::SetupPtr MakeSetup();
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

  mojo::Receiver<chat::mojom::PageHandler> receiver_;
  mojo::Remote<chat::mojom::Page> page_;
  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<TopChromeWebUIController> controller_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  std::unique_ptr<ClaudeClient> client_;
  // Counts Send() calls, so a stopped request's late callbacks are ignored.
  int request_id_ = 0;
  PrefChangeRegistrar pref_registrar_;
  base::CallbackListSubscription active_tab_subscription_;
  base::WeakPtrFactory<ClaudeChatPageHandler> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_PAGE_HANDLER_H_
