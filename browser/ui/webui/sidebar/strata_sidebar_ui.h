// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_SIDEBAR_STRATA_SIDEBAR_UI_H_
#define STRATA_BROWSER_UI_WEBUI_SIDEBAR_STRATA_SIDEBAR_UI_H_

#include <memory>

#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "strata/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace strata {

inline constexpr char kStrataSidebarHost[] = "strata-sidebar";
inline constexpr char kStrataSidebarURL[] = "chrome://strata-sidebar/";

class SidebarPageHandler;
class StrataSidebarUI;

class StrataSidebarUIConfig
    : public content::DefaultWebUIConfig<StrataSidebarUI> {
 public:
  StrataSidebarUIConfig();
};

// chrome://strata-sidebar: the sidebar page shown in the vertical tab strip
// region. Its host (StrataSidebarView) sets the window it belongs to with
// webui::SetBrowserWindowInterface(). See docs/design/sidebar.md.
class StrataSidebarUI : public ui::MojoWebUIController,
                        public sidebar::mojom::PageHandlerFactory {
 public:
  explicit StrataSidebarUI(content::WebUI* web_ui);
  StrataSidebarUI(const StrataSidebarUI&) = delete;
  StrataSidebarUI& operator=(const StrataSidebarUI&) = delete;
  ~StrataSidebarUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::PageHandlerFactory> receiver);

 private:
  // sidebar::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<sidebar::mojom::Page> page,
      mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver) override;

  std::unique_ptr<SidebarPageHandler> page_handler_;
  mojo::Receiver<sidebar::mojom::PageHandlerFactory> page_factory_receiver_{
      this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_SIDEBAR_STRATA_SIDEBAR_UI_H_
