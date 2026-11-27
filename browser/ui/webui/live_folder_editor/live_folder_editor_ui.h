// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_LIVE_FOLDER_EDITOR_LIVE_FOLDER_EDITOR_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_LIVE_FOLDER_EDITOR_LIVE_FOLDER_EDITOR_UI_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "compass/browser/ui/webui/live_folder_editor/live_folder_editor.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "url/gurl.h"

class BrowserWindowInterface;
class Profile;

namespace content {
class WebUI;
}  // namespace content

namespace compass {

class LiveFolderService;
class SidebarNode;
class SpaceModel;

// chrome://compass-live-folder-editor/: the live folder editor, in a bubble
// beside the sidebar (ShowLiveFolderEditorBubble();
// docs/design/live-folders.md, "UI").
inline constexpr char kCompassLiveFolderEditorHost[] =
    "compass-live-folder-editor";

// The editor's URL: for a new folder in |parent_id|, or for folder
// |folder_id| (one of them).
GURL LiveFolderEditorUrl(const std::optional<std::string>& parent_id,
                         const std::optional<std::string>& folder_id);

class CompassLiveFolderEditorUI;

class CompassLiveFolderEditorUIConfig
    : public DefaultTopChromeWebUIConfig<CompassLiveFolderEditorUI> {
 public:
  CompassLiveFolderEditorUIConfig();

  // DefaultTopChromeWebUIConfig:
  bool ShouldAutoResizeHost() override;
};

// Makes and edits the window's space's live folders, and installs and
// removes plugins, for the editor.
class LiveFolderEditorPageHandler
    : public live_folder_editor::mojom::PageHandler {
 public:
  LiveFolderEditorPageHandler(
      mojo::PendingReceiver<live_folder_editor::mojom::PageHandler> receiver,
      CompassLiveFolderEditorUI* controller,
      BrowserWindowInterface* browser);
  LiveFolderEditorPageHandler(const LiveFolderEditorPageHandler&) = delete;
  LiveFolderEditorPageHandler& operator=(const LiveFolderEditorPageHandler&) =
      delete;
  ~LiveFolderEditorPageHandler() override;

  // live_folder_editor::mojom::PageHandler:
  void GetTarget(GetTargetCallback callback) override;
  void GetLivePlugins(GetLivePluginsCallback callback) override;
  void GetLiveFolderSource(const std::string& id,
                           GetLiveFolderSourceCallback callback) override;
  void SaveLiveFolder(sidebar::mojom::LiveFolderEditPtr edit,
                      SaveLiveFolderCallback callback) override;
  void PreviewLivePlugin(const std::string& url,
                         PreviewLivePluginCallback callback) override;
  void InstallLivePlugin(const std::string& url,
                         const std::string& sha256,
                         InstallLivePluginCallback callback) override;
  void RemoveLivePlugin(const std::string& id,
                        RemoveLivePluginCallback callback) override;
  void OpenLivePluginsFolder() override;
  void GetLook(GetLookCallback callback) override;
  void ShowUI() override;
  void Close() override;

 private:
  Profile* profile();
  LiveFolderService* service();
  SpaceModel& space_model();
  SpaceModel& favorites();
  // The window's space's node |id|, or the profile's favorites'.
  SidebarNode* FindNode(const std::string& id);

  mojo::Receiver<live_folder_editor::mojom::PageHandler> receiver_;
  const raw_ptr<CompassLiveFolderEditorUI> controller_;
  const raw_ptr<BrowserWindowInterface> browser_;
  base::WeakPtrFactory<LiveFolderEditorPageHandler> weak_factory_{this};
};

class CompassLiveFolderEditorUI
    : public TopChromeWebUIController,
      public live_folder_editor::mojom::PageHandlerFactory {
 public:
  CompassLiveFolderEditorUI(content::WebUI* web_ui, const GURL& url);
  CompassLiveFolderEditorUI(const CompassLiveFolderEditorUI&) = delete;
  CompassLiveFolderEditorUI& operator=(const CompassLiveFolderEditorUI&) =
      delete;
  ~CompassLiveFolderEditorUI() override;

  void BindInterface(
      mojo::PendingReceiver<live_folder_editor::mojom::PageHandlerFactory>
          receiver);

  // From the URL: where a new folder goes, or the folder edited.
  const std::optional<std::string>& parent_id() const { return parent_id_; }
  const std::optional<std::string>& folder_id() const { return folder_id_; }

  static constexpr std::string_view GetWebUIName() {
    return "CompassLiveFolderEditor";
  }

 private:
  // live_folder_editor::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingReceiver<live_folder_editor::mojom::PageHandler> receiver)
      override;

  std::optional<std::string> parent_id_;
  std::optional<std::string> folder_id_;
  std::unique_ptr<LiveFolderEditorPageHandler> handler_;
  mojo::Receiver<live_folder_editor::mojom::PageHandlerFactory>
      factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_LIVE_FOLDER_EDITOR_LIVE_FOLDER_EDITOR_UI_H_
