// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SPACE_EDITOR_SPACE_EDITOR_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_SPACE_EDITOR_SPACE_EDITOR_UI_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/ui/webui/space_editor/space_editor.mojom.h"
#include "compass/browser/window_sync/space_directory.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace content {
class WebUI;
}  // namespace content

namespace compass {

class SpaceModel;

// chrome://compass-space-editor/?id=<space>: a space's editor, in a bubble
// beside the sidebar (ShowSpaceEditorBubble(); docs/design/spaces.md, "UI").
inline constexpr char kCompassSpaceEditorHost[] = "compass-space-editor";

// The editor's URL for a space.
GURL SpaceEditorUrl(const std::string& space_id);

class CompassSpaceEditorUI;

class CompassSpaceEditorUIConfig
    : public DefaultTopChromeWebUIConfig<CompassSpaceEditorUI> {
 public:
  CompassSpaceEditorUIConfig();

  // DefaultTopChromeWebUIConfig:
  bool ShouldAutoResizeHost() override;
};

// Edits one space, of the window's profile or of another one in the
// switcher (SpaceDirectory), as the sidebar's own handler does.
class SpaceEditorPageHandler : public space_editor::mojom::PageHandler,
                               public SpaceList::Observer,
                               public SpaceDirectory::Observer {
 public:
  SpaceEditorPageHandler(
      mojo::PendingReceiver<space_editor::mojom::PageHandler> receiver,
      mojo::PendingRemote<space_editor::mojom::Page> page,
      CompassSpaceEditorUI* controller,
      BrowserWindowInterface* browser);
  SpaceEditorPageHandler(const SpaceEditorPageHandler&) = delete;
  SpaceEditorPageHandler& operator=(const SpaceEditorPageHandler&) = delete;
  ~SpaceEditorPageHandler() override;

  // space_editor::mojom::PageHandler:
  void GetSpace(GetSpaceCallback callback) override;
  void RenameSpace(const std::string& id, const std::string& name) override;
  void SetSpaceColor(const std::string& id,
                     sidebar::mojom::SpaceColorPtr color) override;
  void SetSpaceGradient(
      const std::string& id,
      std::vector<sidebar::mojom::SpaceColorPtr> gradient) override;
  void SetSpaceIntensity(const std::string& id, double intensity) override;
  void SetSpaceGrain(const std::string& id, double grain) override;
  void SetSpaceAppearance(const std::string& id,
                          sidebar::mojom::SpaceAppearance appearance) override;
  void SetSpaceIcon(const std::string& id,
                    sidebar::mojom::SpaceIconPtr icon) override;
  void SetArchiveAfter(const std::string& id, uint32_t hours) override;
  void SetSpaceProfile(const std::string& id,
                       const std::optional<std::string>& profile_id) override;
  void SetSpaceSynced(const std::string& id, bool synced) override;
  void DeleteSpace(const std::string& id) override;
  void ShareSpace(const std::string& id) override;
  void GetLook(GetLookCallback callback) override;
  void ShowUI() override;
  void Close() override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceRemoved() override;
  void OnSpaceMoved(const SpaceModel& space) override;

  // SpaceDirectory::Observer:
  void OnDirectoryChanged() override;

 private:
  // The space with |id|, of any profile the switcher shows.
  SpaceDirectory::Entry FindAnySpace(const std::string& id);
  // The same, unless the space is a viewer's (sharing).
  SpaceModel* FindEditableSpace(const std::string& id);
  // Null when the window's profile has no directory (Incognito, Guest).
  SpaceDirectory* directory();
  // Tells the page, once for a burst of changes.
  void NotifyChanged();
  void SendChanged();

  mojo::Receiver<space_editor::mojom::PageHandler> receiver_;
  mojo::Remote<space_editor::mojom::Page> page_;
  const raw_ptr<CompassSpaceEditorUI> controller_;
  const raw_ptr<BrowserWindowInterface> browser_;
  bool uses_directory_ = false;
  bool change_pending_ = false;
  base::ScopedObservation<SpaceList, SpaceList::Observer> list_observation_{
      this};
  base::ScopedObservation<SpaceDirectory, SpaceDirectory::Observer>
      directory_observation_{this};
  base::WeakPtrFactory<SpaceEditorPageHandler> weak_factory_{this};
};

class CompassSpaceEditorUI : public TopChromeWebUIController,
                             public space_editor::mojom::PageHandlerFactory {
 public:
  CompassSpaceEditorUI(content::WebUI* web_ui, const GURL& url);
  CompassSpaceEditorUI(const CompassSpaceEditorUI&) = delete;
  CompassSpaceEditorUI& operator=(const CompassSpaceEditorUI&) = delete;
  ~CompassSpaceEditorUI() override;

  void BindInterface(
      mojo::PendingReceiver<space_editor::mojom::PageHandlerFactory> receiver);

  // The space's ID, from the URL.
  const std::string& space_id() const { return space_id_; }

  // Where the bubble points, in screen coordinates: the share bubble opened
  // from it points there too.
  const gfx::Rect& anchor() const { return anchor_; }
  void set_anchor(const gfx::Rect& anchor) { anchor_ = anchor; }

  static constexpr std::string_view GetWebUIName() {
    return "CompassSpaceEditor";
  }

 private:
  // space_editor::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<space_editor::mojom::Page> page,
      mojo::PendingReceiver<space_editor::mojom::PageHandler> receiver)
      override;

  std::string space_id_;
  gfx::Rect anchor_;
  std::unique_ptr<SpaceEditorPageHandler> handler_;
  mojo::Receiver<space_editor::mojom::PageHandlerFactory> factory_receiver_{
      this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SPACE_EDITOR_SPACE_EDITOR_UI_H_
