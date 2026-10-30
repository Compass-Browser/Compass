// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"

#include <utility>

#include "base/functional/bind.h"
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
  pending_page_ = std::move(page);
  pending_receiver_ = std::move(receiver);
  // A preloaded page has no window until a sidebar takes it; its calls wait
  // in the pipe until then. A page opened outside the sidebar host, e.g.
  // typed into the omnibox, never gets one.
  browser_changed_subscription_ = webui::RegisterBrowserWindowInterfaceChanged(
      web_ui()->GetWebContents(),
      base::BindRepeating(&StrataSidebarUI::MaybeCreatePageHandler,
                          base::Unretained(this)));
  MaybeCreatePageHandler();
}

void StrataSidebarUI::MaybeCreatePageHandler() {
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  if (!browser || !pending_receiver_) {
    return;
  }
  browser_changed_subscription_ = {};
  page_handler_ = std::make_unique<SidebarPageHandler>(
      std::move(pending_receiver_), std::move(pending_page_), browser,
      web_ui()->GetWebContents());
  page_handler_->set_tree_shown_callback(base::BindRepeating(
      &StrataSidebarUI::OnTreeShown, base::Unretained(this)));
}

void StrataSidebarUI::RunWhenTreeShown(base::OnceClosure callback) {
  if (tree_shown_) {
    std::move(callback).Run();
    return;
  }
  tree_shown_callbacks_.push_back(std::move(callback));
}

void StrataSidebarUI::OnTreeShown() {
  tree_shown_ = true;
  for (base::OnceClosure& callback :
       std::exchange(tree_shown_callbacks_, {})) {
    std::move(callback).Run();
  }
}

WEB_UI_CONTROLLER_TYPE_IMPL(StrataSidebarUI)

}  // namespace strata
