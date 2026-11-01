// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_SECTIONS_H_
#define COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_SECTIONS_H_

#include <memory>

#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/ui/webui/sync/compass_sync.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace content {
class WebUI;
class WebUIDataSource;
}  // namespace content

namespace compass {

// The sections' paths: compass://settings/sync and so on, for
// chrome::ShowSettingsSubPage().
inline constexpr char kSyncSettingsSubPage[] = "sync";
inline constexpr char kShortcutsSettingsSubPage[] = "shortcuts";
inline constexpr char kArcImportSettingsSubPage[] = "importArc";

// Compass's sections of chrome://settings: Sync (/sync), Keyboard shortcuts
// (/shortcuts) and Import from Arc (/importArc). settings::SettingsUI owns
// one (patches/chrome-browser-ui-webui-settings-settings_ui.{h,cc}.patch)
// and hands it the sections' interfaces. See docs/design/settings.md.
//
// Only this header is visible to //chrome/browser/ui/webui/settings
// (target :sections); the implementation is in //compass/browser/ui.
class SettingsSections {
 public:
  // Serves the sections' scripts on chrome://settings' |source| under
  // compass/ (its settings_main.ts imports /compass/settings/*_page.js;
  // optimized builds bundle them instead).
  static std::unique_ptr<SettingsSections> Create(
      content::WebUI* web_ui,
      content::WebUIDataSource* source);

  virtual ~SettingsSections() = default;

  virtual void Bind(
      mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver) = 0;
  virtual void Bind(
      mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver) = 0;
  virtual void Bind(
      mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver) = 0;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_SECTIONS_H_
