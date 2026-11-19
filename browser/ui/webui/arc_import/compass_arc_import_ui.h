// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_ARC_IMPORT_COMPASS_ARC_IMPORT_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_ARC_IMPORT_COMPASS_ARC_IMPORT_UI_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

class Profile;

namespace content {
class WebUI;
}  // namespace content

namespace compass {

namespace arc {
struct ArcImportResult;
struct ArcOverview;
}  // namespace arc

namespace browser_import {
struct BrowserImportResult;
struct ProfileOverview;
}  // namespace browser_import

class CompassArcImportUI;

class CompassArcImportUIConfig
    : public content::DefaultWebUIConfig<CompassArcImportUI> {
 public:
  CompassArcImportUIConfig();
};

// The page side of importing from Arc and from other browsers. Owned by
// chrome://compass-import and by chrome://settings (SettingsSections). Lists
// Arc's profiles and spaces, asks to quit Arc, imports the chosen profiles;
// lists other browsers' profiles and imports them into this profile; shows
// the first imported space. See docs/design/arc-import.md and
// docs/design/browser-import.md.
class ArcImportPageHandler : public sidebar::mojom::ArcImportHandler {
 public:
  ArcImportPageHandler(
      content::WebUI* web_ui,
      mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver);
  ArcImportPageHandler(const ArcImportPageHandler&) = delete;
  ArcImportPageHandler& operator=(const ArcImportPageHandler&) = delete;
  ~ArcImportPageHandler() override;

  // sidebar::mojom::ArcImportHandler:
  void GetState(GetStateCallback callback) override;
  void QuitArc(QuitArcCallback callback) override;
  void Import(const std::vector<std::string>& profiles,
              const std::vector<std::string>& spaces,
              ImportCallback callback) override;
  void ImportEasels(ImportEaselsCallback callback) override;
  void OpenImported() override;
  void GetBrowsers(GetBrowsersCallback callback) override;
  void ImportBrowsers(
      std::vector<sidebar::mojom::BrowserImportChoicePtr> choices,
      ImportBrowsersCallback callback) override;

 private:
  void OnOverview(GetStateCallback callback, arc::ArcOverview overview);
  void CheckArcQuit(QuitArcCallback callback, int attempts_left);
  void OnImported(ImportCallback callback, arc::ArcImportResult result);
  void OnEaselsImported(ImportEaselsCallback callback,
                        arc::ArcImportResult result);
  void OnBrowsers(GetBrowsersCallback callback,
                  std::vector<browser_import::ProfileOverview> profiles);
  void OnBrowsersImported(ImportBrowsersCallback callback,
                          browser_import::BrowserImportResult result);

  const raw_ptr<content::WebUI> web_ui_;
  // The first imported space, and its profile.
  base::WeakPtr<Profile> imported_profile_;
  base::Uuid imported_space_;
  base::OneShotTimer quit_timer_;

  mojo::Receiver<sidebar::mojom::ArcImportHandler> receiver_;
  base::WeakPtrFactory<ArcImportPageHandler> weak_factory_{this};
};

// chrome://compass-import (compass://compass-import): "Import from Another
// Browser" alone, as compass://settings/importArc shows it.
class CompassArcImportUI : public ui::MojoWebUIController {
 public:
  explicit CompassArcImportUI(content::WebUI* web_ui);
  CompassArcImportUI(const CompassArcImportUI&) = delete;
  CompassArcImportUI& operator=(const CompassArcImportUI&) = delete;
  ~CompassArcImportUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver);

 private:
  std::unique_ptr<ArcImportPageHandler> handler_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_ARC_IMPORT_COMPASS_ARC_IMPORT_UI_H_
