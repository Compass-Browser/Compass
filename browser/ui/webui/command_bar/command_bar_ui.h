// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_UI_H_

#include <memory>
#include <string_view>

#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "compass/browser/ui/webui/command_bar/command_bar.mojom.h"

namespace compass {

inline constexpr char kCompassCommandBarHost[] = "compass-command-bar";
inline constexpr char kCompassCommandBarURL[] = "chrome://compass-command-bar/";

class CommandBarPageHandler;
class CommandBarUI;

class CommandBarUIConfig : public DefaultTopChromeWebUIConfig<CommandBarUI> {
 public:
  CommandBarUIConfig();

  // DefaultTopChromeWebUIConfig:
  bool ShouldAutoResizeHost() override;
};

// chrome://compass-command-bar: the command bar's page, shown in a bubble by
// compass::CommandBar. See docs/design/command-bar.md.
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
    return "CompassCommandBar";
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

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_COMMAND_BAR_COMMAND_BAR_UI_H_
