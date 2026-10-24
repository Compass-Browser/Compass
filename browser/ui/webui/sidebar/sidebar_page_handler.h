// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
#define STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "strata/browser/ui/webui/sidebar/sidebar.mojom.h"

class BrowserWindowInterface;
class TabStripModel;

namespace strata {

// Serves the sidebar page the tabs of the window it is hosted in, and acts on
// its requests. Slice a of docs/design/sidebar.md: reads TabStripModel
// directly; SpaceModel takes over in slice b.
class SidebarPageHandler : public sidebar::mojom::PageHandler,
                           public TabStripModelObserver {
 public:
  SidebarPageHandler(
      mojo::PendingReceiver<sidebar::mojom::PageHandler> receiver,
      mojo::PendingRemote<sidebar::mojom::Page> page,
      BrowserWindowInterface* browser);
  SidebarPageHandler(const SidebarPageHandler&) = delete;
  SidebarPageHandler& operator=(const SidebarPageHandler&) = delete;
  ~SidebarPageHandler() override;

  // sidebar::mojom::PageHandler:
  void GetTabs(GetTabsCallback callback) override;
  void ActivateTab(int32_t tab_id) override;
  void CloseTab(int32_t tab_id) override;
  void NewTab() override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(TabStripModel* tab_strip_model,
                              const TabStripModelChange& change,
                              const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override;

 private:
  TabStripModel* tab_strip_model();
  std::vector<sidebar::mojom::TabPtr> GetTabList();

  // Returns the index of the tab with |tab_id|, or TabStripModel::kNoTab.
  int GetIndexOfTab(int32_t tab_id);

  void SendTabs();

  const raw_ptr<BrowserWindowInterface> browser_;
  mojo::Receiver<sidebar::mojom::PageHandler> receiver_;
  mojo::Remote<sidebar::mojom::Page> page_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_WEBUI_SIDEBAR_SIDEBAR_PAGE_HANDLER_H_
