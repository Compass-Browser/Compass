// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/android_web_ui_configs.h"

#include <memory>

#include "compass/browser/ui/webui/easel/easel.mojom.h"
#include "compass/browser/ui/webui/easel/easel_ui.h"
#include "compass/browser/ui/webui/reader/reader_untrusted_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/webui_config_map.h"

namespace compass {

void RegisterAndroidWebUIConfigs(content::WebUIConfigMap& map) {
  map.AddUntrustedWebUIConfig(std::make_unique<ReaderUntrustedUIConfig>());
  map.AddWebUIConfig(std::make_unique<easel::EaselUIConfig>());
}

void PopulateAndroidWebUIInterfaceBrokers(
    content::WebUIBrowserInterfaceBrokerRegistry& registry) {
  registry.ForWebUI<easel::EaselUI>().Add<easel::mojom::PageHandlerFactory>();
}

}  // namespace compass
