// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/find_bar/find_bar_ui.h"

#include <memory>
#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "compass/browser/ui/views/find_bar/find_bar.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/webui/webui_util.h"

namespace compass {

FindBarUIConfig::FindBarUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kCompassFindBarHost) {}

FindBarUI::FindBarUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  // Built with the sidebar's resources.
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kCompassFindBarHost);
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_FIND_BAR_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

FindBarUI::~FindBarUI() = default;

void FindBarUI::BindInterface(
    mojo::PendingReceiver<find_bar::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void FindBarUI::CreatePageHandler(
    mojo::PendingRemote<find_bar::mojom::Page> page,
    mojo::PendingReceiver<find_bar::mojom::PageHandler> receiver) {
  // FindBar sets the window before it loads the page. A page opened anywhere
  // else (typed into the omnibox) gets no handler.
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  FindBar* bar = browser ? FindBar::From(browser) : nullptr;
  if (bar && bar->page_contents() == web_ui()->GetWebContents()) {
    bar->BindPage(std::move(page), std::move(receiver));
  }
}

WEB_UI_CONTROLLER_TYPE_IMPL(FindBarUI)

}  // namespace compass
