// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/command_bar/command_bar_ui.h"

#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/favicon_source.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "components/favicon_base/favicon_url_parser.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "compass/browser/ui/views/command_bar/command_bar.h"
#include "compass/browser/ui/webui/command_bar/command_bar_page_handler.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "ui/webui/webui_util.h"

namespace compass {

CommandBarUIConfig::CommandBarUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  kCompassCommandBarHost) {}

bool CommandBarUIConfig::ShouldAutoResizeHost() {
  // The bubble grows and shrinks with the results.
  return true;
}

CommandBarUI::CommandBarUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  // The page is built with the sidebar's resources.
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kCompassCommandBarHost);
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_COMMAND_BAR_HTML);

  content::URLDataSource::Add(
      profile, std::make_unique<FaviconSource>(
                   profile, chrome::FaviconUrlFormat::kFavicon2));
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CommandBarUI::~CommandBarUI() = default;

void CommandBarUI::BindInterface(
    mojo::PendingReceiver<command_bar::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void CommandBarUI::CreatePageHandler(
    mojo::PendingRemote<command_bar::mojom::Page> page,
    mojo::PendingReceiver<command_bar::mojom::PageHandler> receiver) {
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  if (!browser) {
    // Opened outside the command bar, e.g. typed into the omnibox.
    return;
  }
  page_handler_ = std::make_unique<CommandBarPageHandler>(
      std::move(receiver), std::move(page), browser, this);
  if (CommandBar* bar = CommandBar::From(browser)) {
    bar->OnPageHandlerCreated(page_handler_.get());
  }
}

WEB_UI_CONTROLLER_TYPE_IMPL(CommandBarUI)

}  // namespace compass
