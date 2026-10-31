// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/compass_web_ui_registry.h"

#include <memory>

#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/webui_config_map.h"
#include "compass/browser/ui/webui/arc_import/compass_arc_import_ui.h"
#include "compass/browser/ui/webui/command_bar/command_bar.mojom.h"
#include "compass/browser/ui/webui/command_bar/command_bar_ui.h"
#include "compass/browser/ui/webui/placeholder/compass_placeholder_ui.h"
#include "compass/browser/ui/webui/settings/compass_settings_ui.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/ui/webui/sidebar/compass_sidebar_ui.h"

namespace compass {

void RegisterWebUIConfigs(content::WebUIConfigMap& map) {
  map.AddWebUIConfig(std::make_unique<CompassSidebarUIConfig>());
  map.AddWebUIConfig(std::make_unique<CompassPlaceholderUIConfig>());
  map.AddWebUIConfig(std::make_unique<CommandBarUIConfig>());
  map.AddWebUIConfig(std::make_unique<CompassArcImportUIConfig>());
  map.AddWebUIConfig(std::make_unique<CompassSettingsUIConfig>());
}

void PopulateWebUIInterfaceBrokers(
    content::WebUIBrowserInterfaceBrokerRegistry& registry) {
  registry.ForWebUI<CompassSidebarUI>()
      .Add<sidebar::mojom::PageHandlerFactory>();
  registry.ForWebUI<CompassPlaceholderUI>()
      .Add<sidebar::mojom::PlaceholderHandler>();
  registry.ForWebUI<CommandBarUI>()
      .Add<command_bar::mojom::PageHandlerFactory>();
  registry.ForWebUI<CompassArcImportUI>()
      .Add<sidebar::mojom::ArcImportHandler>();
  registry.ForWebUI<CompassSettingsUI>()
      .Add<sidebar::mojom::SettingsHandler>();
}

}  // namespace compass
