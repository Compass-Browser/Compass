// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_PLACEHOLDER_COMPASS_PLACEHOLDER_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_PLACEHOLDER_COMPASS_PLACEHOLDER_UI_H_

#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace compass {

class SidebarNode;
class CompassPlaceholderUI;
class TabStripBridge;

class CompassPlaceholderUIConfig
    : public content::DefaultWebUIConfig<CompassPlaceholderUI> {
 public:
  CompassPlaceholderUIConfig();
};

// chrome://compass-placeholder: shown in a window whose tab moved to another
// window, with a way to bring it back. The TabStripBridge that opened it knows
// which node it stands for. See docs/design/sidebar.md, "Window sync".
class CompassPlaceholderUI : public ui::MojoWebUIController,
                            public sidebar::mojom::PlaceholderHandler {
 public:
  explicit CompassPlaceholderUI(content::WebUI* web_ui);
  CompassPlaceholderUI(const CompassPlaceholderUI&) = delete;
  CompassPlaceholderUI& operator=(const CompassPlaceholderUI&) = delete;
  ~CompassPlaceholderUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::PlaceholderHandler> receiver);

  // sidebar::mojom::PlaceholderHandler:
  void GetInfo(GetInfoCallback callback) override;
  void GetSnapshot(GetSnapshotCallback callback) override;
  void BringHere() override;

 private:
  // The node this placeholder stands for, or null if it isn't one any more.
  SidebarNode* GetNode();
  // The bridge of the window this placeholder is in, or null.
  TabStripBridge* GetBridge();

  mojo::Receiver<sidebar::mojom::PlaceholderHandler> receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_PLACEHOLDER_COMPASS_PLACEHOLDER_UI_H_
