// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_ANDROID_WEB_UI_CONFIGS_H_
#define COMPASS_BROWSER_ANDROID_ANDROID_WEB_UI_CONFIGS_H_

namespace content {
class WebUIBrowserInterfaceBrokerRegistry;
class WebUIConfigMap;
}  // namespace content

namespace compass {

// Compass's WebUI on Android, from RegisterChromeWebUIConfigs() (the
// chrome_web_ui_configs.cc patch): the reader view
// (docs/design/android.md, "Reader mode") and Easels (chrome://easel,
// docs/design/easels.md). The desktop's are RegisterWebUIConfigs()
// (compass_web_ui_registry.h).
void RegisterAndroidWebUIConfigs(content::WebUIConfigMap& map);

// Their Mojo interfaces, from
// PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsAndroid() (the
// chromium_src override of chrome_browser_interface_binders_webui_parts_
// android.cc).
void PopulateAndroidWebUIInterfaceBrokers(
    content::WebUIBrowserInterfaceBrokerRegistry& registry);

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_ANDROID_WEB_UI_CONFIGS_H_
