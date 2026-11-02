// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SIDEBAR_COMPASS_SIDEBAR_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_SIDEBAR_COMPASS_SIDEBAR_UI_H_

#include <memory>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"

#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"

namespace compass {

inline constexpr char kCompassSidebarHost[] = "compass-sidebar";
inline constexpr char kCompassSidebarURL[] = "chrome://compass-sidebar/";

class SidebarPageHandler;
class CompassSidebarUI;

class CompassSidebarUIConfig
    : public content::DefaultWebUIConfig<CompassSidebarUI> {
 public:
  CompassSidebarUIConfig();
};

// chrome://compass-sidebar: the sidebar page shown in the vertical tab strip
// region. Its host (CompassSidebarView) sets the window it belongs to with
// webui::SetBrowserWindowInterface(), and sets it again when the page moves
// to another window. See docs/design/sidebar.md.
// A TopChromeWebUIController, like upstream's tab strip pages: the browser's
// own UI, always at 100% zoom whatever the user's default page zoom.
class CompassSidebarUI : public TopChromeWebUIController,
                        public sidebar::mojom::PageHandlerFactory {
 public:
  explicit CompassSidebarUI(content::WebUI* web_ui);
  CompassSidebarUI(const CompassSidebarUI&) = delete;
  CompassSidebarUI& operator=(const CompassSidebarUI&) = delete;
  ~CompassSidebarUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::PageHandlerFactory> receiver);

  // Runs |callback| once the page has taken its first tree: now if it has.
  void RunWhenTreeShown(base::OnceClosure callback);

  // The page's handler, or null until the page has a window.
  SidebarPageHandler* page_handler() { return page_handler_.get(); }

 private:
  void OnTreeShown();
  // Binds the page handler once the page has asked for it and has a window,
  // and again for the window it moves to (a switch to another profile's
  // space keeps the page, docs/design/profiles-in-place.md).
  void MaybeCreatePageHandler();

  // sidebar::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<sidebar::mojom::Page> page,
      mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver) override;

  // What the page sent, until it has a window.
  mojo::PendingRemote<sidebar::mojom::Page> pending_page_;
  mojo::PendingReceiver<sidebar::mojom::PageHandler> pending_receiver_;
  base::CallbackListSubscription browser_changed_subscription_;
  bool tree_shown_ = false;
  std::vector<base::OnceClosure> tree_shown_callbacks_;
  std::unique_ptr<SidebarPageHandler> page_handler_;
  mojo::Receiver<sidebar::mojom::PageHandlerFactory> page_factory_receiver_{
      this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SIDEBAR_COMPASS_SIDEBAR_UI_H_
