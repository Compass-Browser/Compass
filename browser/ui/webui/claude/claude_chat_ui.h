// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_UI_H_

#include <memory>
#include <string_view>

#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "compass/browser/ui/webui/claude/chat.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace compass::claude {

inline constexpr char kClaudeChatHost[] = "compass-claude";
inline constexpr char kClaudeChatURL[] = "chrome://compass-claude/";

class ClaudeChatPageHandler;
class ClaudeChatUI;

class ClaudeChatUIConfig : public DefaultTopChromeWebUIConfig<ClaudeChatUI> {
 public:
  ClaudeChatUIConfig();
};

// chrome://compass-claude: the chat with Claude, in the window's side panel
// (compass::ClaudePanel). See docs/design/claude.md, "Chat panel".
class ClaudeChatUI : public TopChromeWebUIController,
                     public chat::mojom::PageHandlerFactory {
 public:
  explicit ClaudeChatUI(content::WebUI* web_ui);
  ClaudeChatUI(const ClaudeChatUI&) = delete;
  ClaudeChatUI& operator=(const ClaudeChatUI&) = delete;
  ~ClaudeChatUI() override;

  void BindInterface(
      mojo::PendingReceiver<chat::mojom::PageHandlerFactory> receiver);

  // Null until the page has connected.
  ClaudeChatPageHandler* page_handler() { return page_handler_.get(); }

  static constexpr std::string_view GetWebUIName() { return "CompassClaude"; }

 private:
  // chat::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<chat::mojom::Page> page,
      mojo::PendingReceiver<chat::mojom::PageHandler> receiver) override;

  std::unique_ptr<ClaudeChatPageHandler> page_handler_;
  mojo::Receiver<chat::mojom::PageHandlerFactory> page_factory_receiver_{
      this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_WEBUI_CLAUDE_CLAUDE_CHAT_UI_H_
