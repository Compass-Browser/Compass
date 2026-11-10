// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDERS_UNTRUSTED_UI_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDERS_UNTRUSTED_UI_H_

#include "build/build_config.h"
#include "compass/browser/ui/webui/live_folders/live_folders.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/untrusted_web_ui_controller.h"

namespace compass {

class LiveFoldersUntrustedUI;

#if BUILDFLAG(IS_ANDROID)
// Registers the page's config and Mojo binder, once. The desktop does it with
// Compass's other pages (compass_web_ui_registry.cc, through patches of
// Chrome's registries); Chrome's Android registries have no such hooks, and
// both registries take additions at any time, so LiveFolderHost calls this
// before it makes its page.
void RegisterLiveFoldersUntrustedUI();
#endif

class LiveFoldersUntrustedUIConfig
    : public content::DefaultWebUIConfig<LiveFoldersUntrustedUI> {
 public:
  LiveFoldersUntrustedUIConfig();
};

// chrome-untrusted://live-folders/, the page LiveFolderHost runs plugins in.
// Its CSP lets it load only its own scripts and start Workers from them: no
// network, no eval. Only the host's own WebContents gets a page handler.
class LiveFoldersUntrustedUI
    : public ui::UntrustedWebUIController,
      public live_folders::mojom::PageHandlerFactory {
 public:
  explicit LiveFoldersUntrustedUI(content::WebUI* web_ui);
  LiveFoldersUntrustedUI(const LiveFoldersUntrustedUI&) = delete;
  LiveFoldersUntrustedUI& operator=(const LiveFoldersUntrustedUI&) = delete;
  ~LiveFoldersUntrustedUI() override;

  void BindInterface(
      mojo::PendingReceiver<live_folders::mojom::PageHandlerFactory> receiver);

  // live_folders::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<live_folders::mojom::Page> page,
      mojo::PendingReceiver<live_folders::mojom::PageHandler> handler)
      override;

 private:
  mojo::Receiver<live_folders::mojom::PageHandlerFactory> factory_receiver_{
      this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDERS_UNTRUSTED_UI_H_
