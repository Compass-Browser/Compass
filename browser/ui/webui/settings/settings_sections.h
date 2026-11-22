// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_SECTIONS_H_
#define COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_SECTIONS_H_

#include <memory>

#include "compass/browser/ui/webui/claude/claude.mojom.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/ui/webui/sync/compass_sync.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace content {
class WebUI;
}  // namespace content

namespace compass {

// The sections' paths: compass://settings/sync and so on, for
// chrome::ShowSettingsSubPage(), which opens the settings window there.
inline constexpr char kSyncSettingsSubPage[] = "sync";
inline constexpr char kShortcutsSettingsSubPage[] = "shortcuts";
inline constexpr char kArcImportSettingsSubPage[] = "importArc";
inline constexpr char kClaudeSettingsSubPage[] = "claude";
// Links from Other Apps (docs/design/link-routing.md); "links?add=<domain>"
// opens it with a new rule for the domain being made.
inline constexpr char kLinksSettingsSubPage[] = "links";
// Boosts (docs/design/boosts.md); "boosts?add=<host>" opens the editor on a
// host, "boosts?edit=<id>" on a Boost.
inline constexpr char kBoostsSettingsSubPage[] = "boosts";
// Development Mode (docs/design/dev-mode.md), served by the shortcuts'
// handler.
inline constexpr char kDevModeSettingsSubPage[] = "devMode";

// The handlers of Compass's settings sections (Sync, Keyboard shortcuts,
// Links from Other Apps, Boosts, Plugins and Development Mode through the
// shortcuts' handler, Import, Claude), which chrome://compass-settings binds
// for the settings window (docs/design/settings-window.md). Each section
// binds its own SettingsHandler, and they all stay.
class SettingsSections {
 public:
  // The sections' handlers, for chrome://compass-settings (the settings
  // window, docs/design/settings-window.md).
  static std::unique_ptr<SettingsSections> CreateHandlers(
      content::WebUI* web_ui);

  virtual ~SettingsSections() = default;

  virtual void Bind(
      mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver) = 0;
  virtual void Bind(
      mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver) = 0;
  virtual void Bind(
      mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver) = 0;
  virtual void Bind(
      mojo::PendingReceiver<claude::mojom::SettingsHandler> receiver) = 0;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_SECTIONS_H_
