// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sync/compass_sync_ui.h"

#include <utility>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/ui/webui/sync/compass_sync_page_handler.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "components/device_reauth/device_authenticator.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/webui/webui_util.h"

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "chrome/browser/device_reauth/chrome_device_authenticator_factory.h"
#include "components/device_reauth/device_reauth_metrics_util.h"
#endif

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
      std::move(receiver), std::move(page), service,
      base::BindRepeating(&CompassSyncUI::Reauthenticate,
                          base::Unretained(this)));
}

void CompassSyncUI::Reauthenticate(base::OnceCallback<void(bool)> done) {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if (authenticator_) {
    authenticator_->Cancel();
  }
  authenticator_ = ChromeDeviceAuthenticatorFactory::GetForProfile(
      Profile::FromWebUI(web_ui()),
      web_ui()->GetWebContents()->GetTopLevelNativeWindow(),
      device_reauth::DeviceAuthParams(
          base::Seconds(0), device_reauth::DeviceAuthSource::kPasswordManager));
  if (!authenticator_ ||
      !authenticator_->CanAuthenticateWithBiometricOrScreenLock()) {
    // No Touch ID and no password to ask for: as the password manager does,
    // don't ask.
    std::move(done).Run(true);
    return;
  }
  // macOS shows "Compass is trying to show your sync recovery code."
  authenticator_->AuthenticateWithMessage(u"show your sync recovery code",
                                          std::move(done));
#else
  // No device authentication on this platform (as the password manager).
  std::move(done).Run(true);
#endif
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassSyncUI)

}  // namespace compass
