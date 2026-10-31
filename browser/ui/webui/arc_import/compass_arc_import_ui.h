// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_ARC_IMPORT_COMPASS_ARC_IMPORT_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_ARC_IMPORT_COMPASS_ARC_IMPORT_UI_H_

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

class Profile;

namespace compass {

namespace arc {
struct ArcImportResult;
struct ArcOverview;
}  // namespace arc

class CompassArcImportUI;

class CompassArcImportUIConfig
    : public content::DefaultWebUIConfig<CompassArcImportUI> {
 public:
  CompassArcImportUIConfig();
};

// chrome://compass-import (compass://compass-import): importing from Arc.
// Lists Arc's profiles and spaces, asks to quit Arc, imports the chosen
// profiles and shows the first imported space. See
// docs/design/arc-import.md.
class CompassArcImportUI : public ui::MojoWebUIController,
                           public sidebar::mojom::ArcImportHandler {
 public:
  explicit CompassArcImportUI(content::WebUI* web_ui);
  CompassArcImportUI(const CompassArcImportUI&) = delete;
  CompassArcImportUI& operator=(const CompassArcImportUI&) = delete;
  ~CompassArcImportUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver);

  // sidebar::mojom::ArcImportHandler:
  void GetState(GetStateCallback callback) override;
  void QuitArc(QuitArcCallback callback) override;
  void Import(const std::vector<std::string>& dirs,
              ImportCallback callback) override;
  void OpenImported() override;

 private:
  void OnOverview(GetStateCallback callback, arc::ArcOverview overview);
  void CheckArcQuit(QuitArcCallback callback, int attempts_left);
  void OnImported(ImportCallback callback, arc::ArcImportResult result);

  // The first imported space, and its profile.
  base::WeakPtr<Profile> imported_profile_;
  base::Uuid imported_space_;
  base::OneShotTimer quit_timer_;

  mojo::Receiver<sidebar::mojom::ArcImportHandler> receiver_{this};
  base::WeakPtrFactory<CompassArcImportUI> weak_factory_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_ARC_IMPORT_COMPASS_ARC_IMPORT_UI_H_
