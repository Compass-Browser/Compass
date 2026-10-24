// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_LIVE_TAB_REGISTRY_H_
#define STRATA_BROWSER_WINDOW_SYNC_LIVE_TAB_REGISTRY_H_

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "components/tabs/public/tab_interface.h"

namespace strata {

class SidebarNode;

// Which sidebar nodes have a live tab, and the tab, across all windows of a
// profile (ARCHITECTURE.md §7). Each window's TabStripBridge registers the
// tabs of its strip. A tab keeps its entry while it moves between windows.
class LiveTabRegistry {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // |node| gained or lost its live tab.
    virtual void OnNodeLiveChanged(const SidebarNode& node, bool live) {}
  };

  LiveTabRegistry();
  LiveTabRegistry(const LiveTabRegistry&) = delete;
  LiveTabRegistry& operator=(const LiveTabRegistry&) = delete;
  ~LiveTabRegistry();

  void Register(tabs::TabInterface* tab, SidebarNode* node);
  void Unregister(tabs::TabInterface* tab);

  // The live tab of |node| in any window, or null.
  tabs::TabInterface* GetTab(const SidebarNode* node) const;
  // The node of |tab|, or null.
  SidebarNode* GetNode(const tabs::TabInterface* tab) const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  base::flat_map<tabs::TabHandle, raw_ptr<SidebarNode>> node_for_tab_;
  base::flat_map<const SidebarNode*, tabs::TabHandle> tab_for_node_;
  base::ObserverList<Observer> observers_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_LIVE_TAB_REGISTRY_H_
