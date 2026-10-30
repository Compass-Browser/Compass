// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_COMPASS_WEB_UI_REGISTRY_H_
#define COMPASS_BROWSER_UI_WEBUI_COMPASS_WEB_UI_REGISTRY_H_

namespace content {
class WebUIBrowserInterfaceBrokerRegistry;
class WebUIConfigMap;
}  // namespace content

namespace compass {

// Called from RegisterChromeWebUIConfigs() (see patches/).
void RegisterWebUIConfigs(content::WebUIConfigMap& map);

// Called from PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsDesktop()
// (see patches/).
void PopulateWebUIInterfaceBrokers(
    content::WebUIBrowserInterfaceBrokerRegistry& registry);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_COMPASS_WEB_UI_REGISTRY_H_
