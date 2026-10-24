// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/webui/strata_web_ui_registry.h"

#include <memory>

#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/webui_config_map.h"
#include "strata/browser/ui/webui/command_bar/command_bar.mojom.h"
#include "strata/browser/ui/webui/command_bar/command_bar_ui.h"
#include "strata/browser/ui/webui/placeholder/strata_placeholder_ui.h"
#include "strata/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"

namespace strata {

void RegisterWebUIConfigs(content::WebUIConfigMap& map) {
  map.AddWebUIConfig(std::make_unique<StrataSidebarUIConfig>());
  map.AddWebUIConfig(std::make_unique<StrataPlaceholderUIConfig>());
  map.AddWebUIConfig(std::make_unique<CommandBarUIConfig>());
}

void PopulateWebUIInterfaceBrokers(
    content::WebUIBrowserInterfaceBrokerRegistry& registry) {
  registry.ForWebUI<StrataSidebarUI>()
      .Add<sidebar::mojom::PageHandlerFactory>();
  registry.ForWebUI<StrataPlaceholderUI>()
      .Add<sidebar::mojom::PlaceholderHandler>();
  registry.ForWebUI<CommandBarUI>()
      .Add<command_bar::mojom::PageHandlerFactory>();
}

}  // namespace strata
