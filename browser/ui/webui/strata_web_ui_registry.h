// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_STRATA_WEB_UI_REGISTRY_H_
#define STRATA_BROWSER_UI_WEBUI_STRATA_WEB_UI_REGISTRY_H_

namespace content {
class WebUIBrowserInterfaceBrokerRegistry;
class WebUIConfigMap;
}  // namespace content

namespace strata {

// Called from RegisterChromeWebUIConfigs() (see patches/).
void RegisterWebUIConfigs(content::WebUIConfigMap& map);

// Called from PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsDesktop()
// (see patches/).
void PopulateWebUIInterfaceBrokers(
    content::WebUIBrowserInterfaceBrokerRegistry& registry);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_STRATA_WEB_UI_REGISTRY_H_
