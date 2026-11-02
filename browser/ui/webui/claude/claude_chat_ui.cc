// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/claude/claude_chat_ui.h"

#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "compass/browser/ui/webui/claude/claude_chat_page_handler.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/webui/webui_util.h"

namespace compass::claude {

ClaudeChatUIConfig::ClaudeChatUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme, kClaudeChatHost) {}

ClaudeChatUI::ClaudeChatUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  // Built with the sidebar's resources.
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kClaudeChatHost);
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_CLAUDE_CHAT_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

ClaudeChatUI::~ClaudeChatUI() = default;

void ClaudeChatUI::BindInterface(
    mojo::PendingReceiver<chat::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void ClaudeChatUI::CreatePageHandler(
    mojo::PendingRemote<chat::mojom::Page> page,
    mojo::PendingReceiver<chat::mojom::PageHandler> receiver) {
  // The side panel sets the window before it loads the page. A page opened
  // anywhere else (typed into the omnibox) gets no handler.
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  if (!browser) {
    return;
  }
  page_handler_ = std::make_unique<ClaudeChatPageHandler>(
      std::move(receiver), std::move(page), browser, this);
}

WEB_UI_CONTROLLER_TYPE_IMPL(ClaudeChatUI)

}  // namespace compass::claude
