// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Compass's WebUI on Android gets its Mojo interfaces where upstream's
// Android WebUI does (chrome://easel's, docs/design/easels.md). The desktop
// adds them from the desktop parts (the
// chrome_browser_interface_binders_webui_parts_desktop.cc patch).

#include "compass/browser/android/android_web_ui_configs.h"

#define PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsAndroid \
  PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsAndroid_ChromiumImpl
#include "src/chrome/browser/chrome_browser_interface_binders_webui_parts_android.cc"
#undef PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsAndroid

namespace chrome::internal {

void PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsAndroid(
    content::WebUIBrowserInterfaceBrokerRegistry& registry);

void PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsAndroid(
    content::WebUIBrowserInterfaceBrokerRegistry& registry) {
  PopulateChromeWebUIFrameInterfaceBrokersTrustedPartsAndroid_ChromiumImpl(
      registry);
  compass::PopulateAndroidWebUIInterfaceBrokers(registry);
}

}  // namespace chrome::internal
