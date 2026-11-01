// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WINDOW_SYNC_LIVE_TAB_REGISTRY_H_
#define COMPASS_BROWSER_WINDOW_SYNC_LIVE_TAB_REGISTRY_H_

#include <memory>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "components/tabs/public/tab_interface.h"

namespace content {
class WebContents;
}

namespace compass {

class SidebarNode;

// Which sidebar nodes have a live tab, and the tab, across all windows of a
// profile (ARCHITECTURE.md §7). Each window's TabStripBridge registers the
// tabs of its strip. A tab keeps its entry while it moves between windows.
// It also follows the live tabs' sound, for the sidebar's speaker icons
// (docs/design/sidebar.md, "Sound").
class LiveTabRegistry {
 public:
  // What a node's tab sounds like.
  enum class AudioState {
    // Silent, or no live tab.
    kNone,
    // Playing sound now, or a moment ago (RecentlyAudibleHelper).
    kAudible,
    // Muted, playing or not.
    kMuted,
  };

  class Observer : public base::CheckedObserver {
   public:
    // |node| gained or lost its live tab.
    virtual void OnNodeLiveChanged(const SidebarNode& node, bool live) {}
    // GetAudioState(|node|) changed.
    virtual void OnNodeAudioChanged(const SidebarNode& node) {}
    // The contents of |node|'s live tab were muted or unmuted, or a muted
    // tab gained |node|, which isn't muted(). Not called for the registry's
    // own muting (see Register()).
    virtual void OnNodeMutingChanged(const SidebarNode& node, bool muted) {}
  };

  LiveTabRegistry();
  LiveTabRegistry(const LiveTabRegistry&) = delete;
  LiveTabRegistry& operator=(const LiveTabRegistry&) = delete;
  ~LiveTabRegistry();

  // A muted() |node| mutes |tab|, and the contents a discard swaps in
  // (docs/design/sidebar.md, "Sound").
  void Register(tabs::TabInterface* tab, SidebarNode* node);
  void Unregister(tabs::TabInterface* tab);

  // The live tab of |node| in any window, or null.
  tabs::TabInterface* GetTab(const SidebarNode* node) const;
  // The node of |tab|, or null.
  SidebarNode* GetNode(const tabs::TabInterface* tab) const;
  // kNone for a node without a live tab.
  AudioState GetAudioState(const SidebarNode* node) const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  class AudioWatch;

  // A registered tab's sound changed.
  void OnAudioChanged(tabs::TabInterface* tab);
  void OnMutingChanged(tabs::TabInterface* tab, bool muted);
  // Mutes |contents| if |tab|'s node is muted().
  void ApplyNodeMuted(tabs::TabInterface* tab, content::WebContents* contents);

  base::flat_map<tabs::TabHandle, raw_ptr<SidebarNode>> node_for_tab_;
  base::flat_map<const SidebarNode*, tabs::TabHandle> tab_for_node_;
  base::flat_map<tabs::TabHandle, std::unique_ptr<AudioWatch>> audio_;
  base::ObserverList<Observer> observers_;
  // Set while ApplyNodeMuted() mutes a tab.
  bool applying_muted_ = false;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_WINDOW_SYNC_LIVE_TAB_REGISTRY_H_
