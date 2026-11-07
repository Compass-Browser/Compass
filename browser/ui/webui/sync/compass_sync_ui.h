// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_UI_H_

#include <memory>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "compass/browser/ui/webui/sync/compass_sync.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace content {
class WebUI;
}  // namespace content

namespace device_reauth {
class DeviceAuthenticator;
}  // namespace device_reauth

namespace compass {

inline constexpr char kCompassSyncHost[] = "compass-sync";

class CompassSyncPageHandler;
class CompassSyncUI;

class CompassSyncUIConfig : public content::DefaultWebUIConfig<CompassSyncUI> {
 public:
  CompassSyncUIConfig();
};

// The page side of sync: makes the page's handler. Owned by
// chrome://compass-sync and by chrome://settings (SettingsSections). See
// docs/design/sync.md, "UI".
class SyncPageHandlerFactory : public sync::mojom::PageHandlerFactory {
 public:
  SyncPageHandlerFactory(
      content::WebUI* web_ui,
      mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver);
  SyncPageHandlerFactory(const SyncPageHandlerFactory&) = delete;
  SyncPageHandlerFactory& operator=(const SyncPageHandlerFactory&) = delete;
  ~SyncPageHandlerFactory() override;

 private:
  // sync::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<sync::mojom::Page> page,
      mojo::PendingReceiver<sync::mojom::PageHandler> receiver) override;

  // Touch ID or the Mac's password, as the password manager asks before
  // showing passwords.
  void Reauthenticate(base::OnceCallback<void(bool)> done);

  const raw_ptr<content::WebUI> web_ui_;
  std::unique_ptr<device_reauth::DeviceAuthenticator> authenticator_;
  std::unique_ptr<CompassSyncPageHandler> page_handler_;
  mojo::Receiver<sync::mojom::PageHandlerFactory> receiver_;
};

// chrome://compass-sync (compass://compass-sync): sync alone, as
// compass://settings/sync shows it: turning sync on and off, pairing
// devices, the devices, what syncs, the recovery code.
class CompassSyncUI : public ui::MojoWebUIController {
 public:
  explicit CompassSyncUI(content::WebUI* web_ui);
  CompassSyncUI(const CompassSyncUI&) = delete;
  CompassSyncUI& operator=(const CompassSyncUI&) = delete;
  ~CompassSyncUI() override;

  void BindInterface(
      mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver);

 private:
  std::unique_ptr<SyncPageHandlerFactory> factory_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_UI_H_
