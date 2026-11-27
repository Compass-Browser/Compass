// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_ARCHIVE_ARCHIVE_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_ARCHIVE_ARCHIVE_UI_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/webui/archive/archive.mojom.h"
#include "compass/browser/window_sync/window_features.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace content {
class WebUI;
}  // namespace content

namespace compass {

// chrome://compass-archive/: the window's space's archive, in a bubble
// beside the sidebar (ShowArchiveBubble(); docs/design/sidebar.md, "Today
// auto-archive").
inline constexpr char kCompassArchiveHost[] = "compass-archive";

GURL ArchiveUrl();

class CompassArchiveUI;

class CompassArchiveUIConfig
    : public DefaultTopChromeWebUIConfig<CompassArchiveUI> {
 public:
  CompassArchiveUIConfig();

  // DefaultTopChromeWebUIConfig:
  bool ShouldAutoResizeHost() override;
};

// Lists, restores and removes the window's space's archived entries,
// following the window to the space it shows.
class ArchivePageHandler : public archive::mojom::PageHandler,
                           public SpaceModel::Observer,
                           public WindowFeatures::Observer {
 public:
  ArchivePageHandler(mojo::PendingReceiver<archive::mojom::PageHandler> receiver,
                     mojo::PendingRemote<archive::mojom::Page> page,
                     CompassArchiveUI* controller,
                     BrowserWindowInterface* browser);
  ArchivePageHandler(const ArchivePageHandler&) = delete;
  ArchivePageHandler& operator=(const ArchivePageHandler&) = delete;
  ~ArchivePageHandler() override;

  // archive::mojom::PageHandler:
  void GetArchive(GetArchiveCallback callback) override;
  void RestoreArchived(const std::string& id) override;
  void RemoveArchived(const std::string& id) override;
  void ClearArchive() override;
  void GetLook(GetLookCallback callback) override;
  void ShowUI() override;
  void Close() override;

  // SpaceModel::Observer:
  void OnArchiveChanged(const SpaceModel& space) override;

  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;
  void OnSwappedAway() override;

 private:
  void StopObservingSpace();

  mojo::Receiver<archive::mojom::PageHandler> receiver_;
  mojo::Remote<archive::mojom::Page> page_;
  const raw_ptr<CompassArchiveUI> controller_;
  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<WindowFeatures> features_;
  // The window's space; null once the window swapped away.
  raw_ptr<SpaceModel> observed_space_ = nullptr;
};

class CompassArchiveUI : public TopChromeWebUIController,
                         public archive::mojom::PageHandlerFactory {
 public:
  CompassArchiveUI(content::WebUI* web_ui, const GURL& url);
  CompassArchiveUI(const CompassArchiveUI&) = delete;
  CompassArchiveUI& operator=(const CompassArchiveUI&) = delete;
  ~CompassArchiveUI() override;

  void BindInterface(
      mojo::PendingReceiver<archive::mojom::PageHandlerFactory> receiver);

  static constexpr std::string_view GetWebUIName() { return "CompassArchive"; }

 private:
  // archive::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<archive::mojom::Page> page,
      mojo::PendingReceiver<archive::mojom::PageHandler> receiver) override;

  std::unique_ptr<ArchivePageHandler> handler_;
  mojo::Receiver<archive::mojom::PageHandlerFactory> factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_ARCHIVE_ARCHIVE_UI_H_
