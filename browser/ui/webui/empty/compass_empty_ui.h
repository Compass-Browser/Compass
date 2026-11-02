// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_EMPTY_COMPASS_EMPTY_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_EMPTY_COMPASS_EMPTY_UI_H_

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/window_sync/window_features.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace compass {

class CompassEmptyUI;

class CompassEmptyUIConfig
    : public content::DefaultWebUIConfig<CompassEmptyUI> {
 public:
  CompassEmptyUIConfig();
};

// chrome://compass-empty: the page a window with nothing else to show shows,
// under its command bar (window_sync/empty_tab.h).
class CompassEmptyUI : public ui::MojoWebUIController,
                       public sidebar::mojom::EmptyPageHandler,
                       public WindowFeatures::Observer,
                       public SpaceList::Observer {
 public:
  explicit CompassEmptyUI(content::WebUI* web_ui);
  CompassEmptyUI(const CompassEmptyUI&) = delete;
  CompassEmptyUI& operator=(const CompassEmptyUI&) = delete;
  ~CompassEmptyUI() override;

  void BindInterface(
      mojo::PendingReceiver<sidebar::mojom::EmptyPageHandler> receiver);

  // sidebar::mojom::EmptyPageHandler:
  void ShowCommandBar() override;
  void SetPage(
      mojo::PendingRemote<sidebar::mojom::EmptyPage> page) override;

 private:
  // WindowFeatures::Observer:
  void OnSpaceSwitched() override;
  void OnSwappedAway() override;
  // SpaceList::Observer:
  void OnSpaceUpdated(const SpaceModel& space) override;

  // Follows the window the tab is in, if any.
  void ObserveWindow(tabs::TabInterface* tab);
  void OnTabWillDetach(tabs::TabInterface* tab,
                       tabs::TabInterface::DetachReason reason);
  void SendTint();

  mojo::Receiver<sidebar::mojom::EmptyPageHandler> receiver_{this};
  mojo::Remote<sidebar::mojom::EmptyPage> page_;
  raw_ptr<WindowFeatures> features_ = nullptr;
  base::ScopedObservation<WindowFeatures, WindowFeatures::Observer>
      features_observation_{this};
  base::ScopedObservation<SpaceList, SpaceList::Observer>
      space_list_observation_{this};
  base::CallbackListSubscription did_insert_subscription_;
  base::CallbackListSubscription will_detach_subscription_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_EMPTY_COMPASS_EMPTY_UI_H_
