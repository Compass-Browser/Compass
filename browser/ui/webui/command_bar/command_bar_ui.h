// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_UI_H_
#define STRATA_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_UI_H_

#include <memory>
#include <string_view>

#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "strata/browser/ui/webui/command_bar/command_bar.mojom.h"

namespace strata {

inline constexpr char kStrataCommandBarHost[] = "strata-command-bar";
inline constexpr char kStrataCommandBarURL[] = "chrome://strata-command-bar/";

class CommandBarPageHandler;
class CommandBarUI;

class CommandBarUIConfig : public DefaultTopChromeWebUIConfig<CommandBarUI> {
 public:
  CommandBarUIConfig();

  // DefaultTopChromeWebUIConfig:
  bool ShouldAutoResizeHost() override;
};

// chrome://strata-command-bar: the command bar's page, shown in a bubble by
// strata::CommandBar. See docs/design/command-bar.md.
class CommandBarUI : public TopChromeWebUIController,
                     public command_bar::mojom::PageHandlerFactory {
 public:
  explicit CommandBarUI(content::WebUI* web_ui);
  CommandBarUI(const CommandBarUI&) = delete;
  CommandBarUI& operator=(const CommandBarUI&) = delete;
  ~CommandBarUI() override;

  void BindInterface(
      mojo::PendingReceiver<command_bar::mojom::PageHandlerFactory> receiver);

  // Null until the page has connected.
  CommandBarPageHandler* page_handler() { return page_handler_.get(); }

  static constexpr std::string_view GetWebUIName() {
    return "StrataCommandBar";
  }

 private:
  // command_bar::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<command_bar::mojom::Page> page,
      mojo::PendingReceiver<command_bar::mojom::PageHandler> receiver) override;

  std::unique_ptr<CommandBarPageHandler> page_handler_;
  mojo::Receiver<command_bar::mojom::PageHandlerFactory>
      page_factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_UI_H_
