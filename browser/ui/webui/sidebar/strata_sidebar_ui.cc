// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"

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
#include "strata/browser/ui/webui/sidebar/sidebar_page_handler.h"
#include "strata/grit/strata_sidebar_resources.h"
#include "strata/grit/strata_sidebar_resources_map.h"
#include "ui/webui/webui_util.h"

namespace strata {

StrataSidebarUIConfig::StrataSidebarUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kStrataSidebarHost) {}

StrataSidebarUI::StrataSidebarUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kStrataSidebarHost);
  webui::SetupWebUIDataSource(source, kStrataSidebarResources,
                              IDR_STRATA_SIDEBAR_STRATA_SIDEBAR_HTML);

  content::URLDataSource::Add(
      profile, std::make_unique<FaviconSource>(
                   profile, chrome::FaviconUrlFormat::kFavicon2));
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

StrataSidebarUI::~StrataSidebarUI() = default;

void StrataSidebarUI::BindInterface(
    mojo::PendingReceiver<sidebar::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void StrataSidebarUI::CreatePageHandler(
    mojo::PendingRemote<sidebar::mojom::Page> page,
    mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver) {
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  if (!browser) {
    // Opened outside the sidebar host, e.g. typed into the omnibox.
    return;
  }
  page_handler_ = std::make_unique<SidebarPageHandler>(
      std::move(receiver), std::move(page), browser);
}

WEB_UI_CONTROLLER_TYPE_IMPL(StrataSidebarUI)

}  // namespace strata
