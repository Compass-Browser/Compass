// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SETTINGS_COMPASS_SETTINGS_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_SETTINGS_COMPASS_SETTINGS_UI_H_

#include <optional>
#include <string>
#include <vector>

#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace compass {

class CompassSettingsUI;

class CompassSettingsUIConfig
    : public content::DefaultWebUIConfig<CompassSettingsUI> {
 public:
  CompassSettingsUIConfig();
};

// A shortcut the settings page lists, with its keys now.
struct ListedShortcut {
  ListedShortcut();
  ListedShortcut(const ListedShortcut&);
  ListedShortcut& operator=(const ListedShortcut&);
  ~ListedShortcut();

  std::string id;
  std::string section;
  std::string label;
  std::vector<ui::Accelerator> keys;
  std::vector<ui::Accelerator> default_keys;
  bool customized = false;
};

// Every customizable shortcut, in the page's order: the browser commands
// (macOS), click-through picture-in-picture (macOS), then the spaces.
std::vector<ListedShortcut> ListShortcuts();

// chrome://compass-settings (compass://compass-settings): Compass's own
// settings. For now its shortcuts: each one's keys, recorded by pressing
// them. See docs/design/shortcuts.md "Customizing".
class CompassSettingsUI : public ui::MojoWebUIController,
                          public sidebar::mojom::SettingsHandler {
 public:
  explicit CompassSettingsUI(content::WebUI* web_ui);
  CompassSettingsUI(const CompassSettingsUI&) = delete;
  CompassSettingsUI& operator=(const CompassSettingsUI&) = delete;
  ~CompassSettingsUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver);

  // sidebar::mojom::SettingsHandler:
  void GetShortcuts(GetShortcutsCallback callback) override;
  void SetRecording(bool recording) override;
  void CheckShortcut(const std::string& id,
                     sidebar::mojom::ShortcutKeysPtr keys,
                     CheckShortcutCallback callback) override;
  void SetShortcut(const std::string& id,
                   sidebar::mojom::ShortcutKeysPtr keys,
                   SetShortcutCallback callback) override;
  void RemoveShortcut(const std::string& id,
                      RemoveShortcutCallback callback) override;
  void ResetShortcut(const std::string& id,
                     ResetShortcutCallback callback) override;
  void ResetAllShortcuts(ResetAllShortcutsCallback callback) override;

 private:
  bool recording_ = false;
  mojo::Receiver<sidebar::mojom::SettingsHandler> receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SETTINGS_COMPASS_SETTINGS_UI_H_
