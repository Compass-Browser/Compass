// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sync/compass_sync_ui.h"

#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/ui/webui/sync/compass_sync_page_handler.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/webui/webui_util.h"

namespace compass {

CompassSyncUIConfig::CompassSyncUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kCompassSyncHost) {}

CompassSyncUI::CompassSyncUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kCompassSyncHost);
  // Shares the sidebar's resource pak; only the default page differs.
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_COMPASS_SYNC_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassSyncUI::~CompassSyncUI() = default;

void CompassSyncUI::BindInterface(
    mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void CompassSyncUI::CreatePageHandler(
    mojo::PendingRemote<sync::mojom::Page> page,
    mojo::PendingReceiver<sync::mojom::PageHandler> receiver) {
  sync::CompassSyncService* service =
      sync::CompassSyncServiceFactory::GetForProfile(
          Profile::FromWebUI(web_ui()));
  if (!service) {
    // Incognito and guest profiles don't sync.
    return;
  }
  page_handler_ = std::make_unique<CompassSyncPageHandler>(
      std::move(receiver), std::move(page), service);
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassSyncUI)

}  // namespace compass
