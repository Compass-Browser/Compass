// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/live_tab_registry.h"

#include "base/check.h"
#include "compass/browser/sidebar/sidebar_node.h"

namespace compass {

LiveTabRegistry::LiveTabRegistry() = default;

LiveTabRegistry::~LiveTabRegistry() = default;

void LiveTabRegistry::Register(tabs::TabInterface* tab, SidebarNode* node) {
  CHECK(tab);
  CHECK(node);
  CHECK(!GetTab(node));
  node_for_tab_[tab->GetHandle()] = node;
  tab_for_node_[node] = tab->GetHandle();
  for (auto& observer : observers_) {
    observer.OnNodeLiveChanged(*node, true);
  }
}

void LiveTabRegistry::Unregister(tabs::TabInterface* tab) {
  auto it = node_for_tab_.find(tab->GetHandle());
  if (it == node_for_tab_.end()) {
    return;
  }
  SidebarNode* node = it->second;
  node_for_tab_.erase(it);
  tab_for_node_.erase(node);
  for (auto& observer : observers_) {
    observer.OnNodeLiveChanged(*node, false);
  }
}

tabs::TabInterface* LiveTabRegistry::GetTab(const SidebarNode* node) const {
  auto it = tab_for_node_.find(node);
  return it == tab_for_node_.end() ? nullptr : it->second.Get();
}

SidebarNode* LiveTabRegistry::GetNode(const tabs::TabInterface* tab) const {
  if (!tab) {
    return nullptr;
  }
  auto it = node_for_tab_.find(tab->GetHandle());
  return it == node_for_tab_.end() ? nullptr : it->second.get();
}

void LiveTabRegistry::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void LiveTabRegistry::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

}  // namespace compass
