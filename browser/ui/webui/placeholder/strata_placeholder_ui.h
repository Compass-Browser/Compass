// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_PLACEHOLDER_STRATA_PLACEHOLDER_UI_H_
#define STRATA_BROWSER_UI_WEBUI_PLACEHOLDER_STRATA_PLACEHOLDER_UI_H_

#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "strata/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace strata {

class SidebarNode;
class StrataPlaceholderUI;

class StrataPlaceholderUIConfig
    : public content::DefaultWebUIConfig<StrataPlaceholderUI> {
 public:
  StrataPlaceholderUIConfig();
};

// chrome://strata-placeholder: shown in a window whose tab moved to another
// window, with a way to bring it back. The TabStripBridge that opened it knows
// which node it stands for. See docs/design/sidebar.md, "Window sync".
class StrataPlaceholderUI : public ui::MojoWebUIController,
                            public sidebar::mojom::PlaceholderHandler {
 public:
  explicit StrataPlaceholderUI(content::WebUI* web_ui);
  StrataPlaceholderUI(const StrataPlaceholderUI&) = delete;
  StrataPlaceholderUI& operator=(const StrataPlaceholderUI&) = delete;
  ~StrataPlaceholderUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::PlaceholderHandler> receiver);

  // sidebar::mojom::PlaceholderHandler:
  void GetInfo(GetInfoCallback callback) override;
  void BringHere() override;

 private:
  // The node this placeholder stands for, or null if it isn't one any more.
  SidebarNode* GetNode();

  mojo::Receiver<sidebar::mojom::PlaceholderHandler> receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_PLACEHOLDER_STRATA_PLACEHOLDER_UI_H_
