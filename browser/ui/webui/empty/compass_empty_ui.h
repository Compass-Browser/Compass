// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_EMPTY_COMPASS_EMPTY_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_EMPTY_COMPASS_EMPTY_UI_H_

#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace compass {

class CompassEmptyUI;

class CompassEmptyUIConfig
    : public content::DefaultWebUIConfig<CompassEmptyUI> {
 public:
  CompassEmptyUIConfig();
};

// chrome://compass-empty: the page a window with nothing else to show shows,
// under its command bar (window_sync/empty_tab.h).
class CompassEmptyUI : public ui::MojoWebUIController,
                       public sidebar::mojom::EmptyPageHandler {
 public:
  explicit CompassEmptyUI(content::WebUI* web_ui);
  CompassEmptyUI(const CompassEmptyUI&) = delete;
  CompassEmptyUI& operator=(const CompassEmptyUI&) = delete;
  ~CompassEmptyUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::EmptyPageHandler> receiver);

  // sidebar::mojom::EmptyPageHandler:
  void ShowCommandBar() override;

 private:
  mojo::Receiver<sidebar::mojom::EmptyPageHandler> receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_EMPTY_COMPASS_EMPTY_UI_H_
