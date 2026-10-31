// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_UI_H_

#include <memory>

#include "compass/browser/ui/webui/sync/compass_sync.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace compass {

inline constexpr char kCompassSyncHost[] = "compass-sync";

class CompassSyncPageHandler;
class CompassSyncUI;

class CompassSyncUIConfig : public content::DefaultWebUIConfig<CompassSyncUI> {
 public:
  CompassSyncUIConfig();
};

// chrome://compass-sync (compass://compass-sync): turning sync on and off,
// and pairing devices. Slice 2's minimal page; see docs/design/sync.md, "UI".
class CompassSyncUI : public ui::MojoWebUIController,
                      public sync::mojom::PageHandlerFactory {
 public:
  explicit CompassSyncUI(content::WebUI* web_ui);
  CompassSyncUI(const CompassSyncUI&) = delete;
  CompassSyncUI& operator=(const CompassSyncUI&) = delete;
  ~CompassSyncUI() override;

  void BindInterface(
      mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver);

 private:
  // sync::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<sync::mojom::Page> page,
      mojo::PendingReceiver<sync::mojom::PageHandler> receiver) override;

  std::unique_ptr<CompassSyncPageHandler> page_handler_;
  mojo::Receiver<sync::mojom::PageHandlerFactory> page_factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SYNC_COMPASS_SYNC_UI_H_
