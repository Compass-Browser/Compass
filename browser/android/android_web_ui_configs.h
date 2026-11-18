// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_ANDROID_WEB_UI_CONFIGS_H_
#define COMPASS_BROWSER_ANDROID_ANDROID_WEB_UI_CONFIGS_H_

namespace content {
class WebUIConfigMap;
}  // namespace content

namespace compass {

// Compass's WebUI on Android, from RegisterChromeWebUIConfigs() (the
// chrome_web_ui_configs.cc patch): the reader view
// (docs/design/android.md, "Reader mode"). The desktop's are
// RegisterWebUIConfigs() (compass_web_ui_registry.h).
void RegisterAndroidWebUIConfigs(content::WebUIConfigMap& map);

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_ANDROID_WEB_UI_CONFIGS_H_
