// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_UI_H_

#include <memory>

#include "compass/browser/ui/webui/easel/easel.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace content {
class WebUI;
}  // namespace content

namespace compass::easel {

// compass://easel maps to it (common/compass_url.cc).
inline constexpr char kEaselHost[] = "easel";

class EaselPageHandler;
class EaselUI;

class EaselUIConfig : public content::DefaultWebUIConfig<EaselUI> {
 public:
  EaselUIConfig();
};

// chrome://easel/ lists the profile's Easels; chrome://easel/<uuid> is a
// board's editor (docs/design/easels.md, "Editor"), on the desktop and on
// Android. chrome://easel/asset/<sha256> serves the boards' images from
// the store.
class EaselUI : public ui::MojoWebUIController,
                public mojom::PageHandlerFactory {
 public:
  explicit EaselUI(content::WebUI* web_ui);
  EaselUI(const EaselUI&) = delete;
  EaselUI& operator=(const EaselUI&) = delete;
  ~EaselUI() override;

  void BindInterface(
      mojo::PendingReceiver<mojom::PageHandlerFactory> receiver);

 private:
  // mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<mojom::Page> page,
      mojo::PendingReceiver<mojom::PageHandler> receiver) override;

  std::unique_ptr<EaselPageHandler> page_handler_;
  mojo::Receiver<mojom::PageHandlerFactory> factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass::easel

#endif  // COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_UI_H_
