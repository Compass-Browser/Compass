// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SETTINGS_COMPASS_SETTINGS_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_SETTINGS_COMPASS_SETTINGS_UI_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "compass/browser/ui/webui/claude/claude.mojom.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/ui/webui/sync/compass_sync.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/webui/mojo_web_ui_controller.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace compass {

class CompassSettingsUI;
class SettingsSections;
class SettingsWindowHandler;
class SpaceList;

namespace boosts {
class BoostStore;
}  // namespace boosts

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

// The page side of the keyboard shortcuts section: each shortcut's keys,
// recorded by pressing them. Owned by chrome://compass-settings and by
// chrome://settings (SettingsSections). See docs/design/shortcuts.md
// "Customizing".
// Keyboard shortcuts (docs/design/shortcuts.md) and, for |profile|, links
// from other apps (docs/design/link-routing.md).
class ShortcutsPageHandler : public sidebar::mojom::SettingsHandler,
                             public ui::SelectFileDialog::Listener {
 public:
  // |web_contents|: the page's, for the file dialogs Boosts' sharing opens.
  ShortcutsPageHandler(
      Profile* profile,
      content::WebContents* web_contents,
      mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver);
  ShortcutsPageHandler(const ShortcutsPageHandler&) = delete;
  ShortcutsPageHandler& operator=(const ShortcutsPageHandler&) = delete;
  ~ShortcutsPageHandler() override;

  // sidebar::mojom::SettingsHandler:
  void GetShortcuts(GetShortcutsCallback callback) override;
  void GetCtrlTabRecent(GetCtrlTabRecentCallback callback) override;
  void SetCtrlTabRecent(bool enabled,
                        SetCtrlTabRecentCallback callback) override;
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
  void GetLinkRules(GetLinkRulesCallback callback) override;
  void SetLinkRule(const std::string& domain,
                   const std::string& space_id,
                   const std::string& replacing,
                   SetLinkRuleCallback callback) override;
  void RemoveLinkRule(const std::string& domain,
                      RemoveLinkRuleCallback callback) override;
  void GetDevModeHosts(GetDevModeHostsCallback callback) override;
  void AddDevModeHost(const std::string& host,
                      AddDevModeHostCallback callback) override;
  void RemoveDevModeHost(const std::string& host,
                         RemoveDevModeHostCallback callback) override;
  // Plugins (plugin_settings.cc).
  void GetPlugins(GetPluginsCallback callback) override;
  void SetTabPreviewsEnabled(bool enabled,
                             SetTabPreviewsEnabledCallback callback) override;
  void SetPluginPreviewEnabled(
      const std::string& id,
      bool enabled,
      SetPluginPreviewEnabledCallback callback) override;
  void SetPluginSessionGranted(
      const std::string& id,
      bool granted,
      SetPluginSessionGrantedCallback callback) override;
  void SetPluginPreviewConfig(const std::string& id,
                              const std::string& config_json,
                              SetPluginPreviewConfigCallback callback) override;
  void GetPluginSource(const std::string& id,
                       GetPluginSourceCallback callback) override;
  void PreviewPlugin(const std::string& url,
                     PreviewPluginCallback callback) override;
  void InstallPlugin(const std::string& url,
                     const std::string& sha256,
                     InstallPluginCallback callback) override;
  void RemovePlugin(const std::string& id,
                    RemovePluginCallback callback) override;
  void OpenPluginsFolder() override;
  void MakePluginWithClaude() override;
  void GetBoosts(GetBoostsCallback callback) override;
  void SaveBoost(sidebar::mojom::BoostPtr boost,
                 SaveBoostCallback callback) override;
  void SetBoostEnabled(const std::string& id,
                       bool enabled,
                       SetBoostEnabledCallback callback) override;
  void DeleteBoost(const std::string& id,
                   DeleteBoostCallback callback) override;
  void GetBoostShareLink(const std::string& id,
                         GetBoostShareLinkCallback callback) override;
  void CopyBoostShareLink(const std::string& id,
                          CopyBoostShareLinkCallback callback) override;
  void SaveBoostFile(const std::string& id,
                     SaveBoostFileCallback callback) override;
  void ChooseBoostFile(ChooseBoostFileCallback callback) override;
  void PreviewSharedBoost(const std::string& payload,
                          PreviewSharedBoostCallback callback) override;
  void AddSharedBoost(const std::string& payload,
                      AddSharedBoostCallback callback) override;

  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

 private:
  // The share link for |id|, if there's such a Boost and the link isn't too
  // long.
  std::string ShareLinkFor(const std::string& id);
  void OnBoostFileRead(std::string contents);
  // The profile's spaces, if it has them (not off the record: the rules
  // are the original profile's).
  SpaceList* GetSpaceList();
  // The profile's Boosts, once loaded; null for none (Guest).
  boosts::BoostStore* GetBoostStore();

  const raw_ptr<Profile> profile_;
  const raw_ptr<content::WebContents> web_contents_;
  bool recording_ = false;
  // The file dialog open for sharing, one at a time: saving |file_json_|,
  // or choosing a file to import.
  scoped_refptr<ui::SelectFileDialog> file_dialog_;
  std::string file_json_;
  SaveBoostFileCallback save_callback_;
  ChooseBoostFileCallback choose_callback_;
  mojo::Receiver<sidebar::mojom::SettingsHandler> receiver_;
  base::WeakPtrFactory<ShortcutsPageHandler> weak_factory_{this};
};

// chrome://compass-settings (compass://compass-settings): Compass's settings,
// shown in the settings window (docs/design/settings-window.md) with the
// panes as toolbar tabs. Also serves the standalone sections the end-to-end
// steps and browser tests drive (shortcuts.html, boosts.html).
class CompassSettingsUI : public ui::MojoWebUIController,
                          public sidebar::mojom::SettingsWindowHandlerFactory {
 public:
  explicit CompassSettingsUI(content::WebUI* web_ui);
  CompassSettingsUI(const CompassSettingsUI&) = delete;
  CompassSettingsUI& operator=(const CompassSettingsUI&) = delete;
  ~CompassSettingsUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::SettingsWindowHandlerFactory>
          receiver);
  // The sections' (SettingsSections, as chrome://settings has them).
  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver);
  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver);
  void BindInterface(
      mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver);
  void BindInterface(
      mojo::PendingReceiver<claude::mojom::SettingsHandler> receiver);

  // sidebar::mojom::SettingsWindowHandlerFactory:
  void CreateHandler(
      mojo::PendingRemote<sidebar::mojom::SettingsWindowPage> page,
      mojo::PendingReceiver<sidebar::mojom::SettingsWindowHandler> handler)
      override;

 private:
  std::unique_ptr<SettingsSections> sections_;
  std::unique_ptr<SettingsWindowHandler> window_handler_;
  mojo::Receiver<sidebar::mojom::SettingsWindowHandlerFactory>
      window_factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SETTINGS_COMPASS_SETTINGS_UI_H_
