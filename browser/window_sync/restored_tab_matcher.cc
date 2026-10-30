// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/restored_tab_matcher.h"

#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The first unloaded tab under |node| at |url|, depth first.
SidebarNode* FindUnloaded(const LiveTabRegistry& registry,
                          SidebarNode* node,
                          const GURL& url) {
  if (node->type() == SidebarNode::Type::kTab && node->url() == url &&
      !registry.GetTab(node)) {
    return node;
  }
  for (const auto& child : node->children()) {
    if (SidebarNode* found = FindUnloaded(registry, child.get(), url)) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

RestoredTabMatcher::RestoredTabMatcher(SpaceModel* model,
                                       SpaceModel* favorites,
                                       LiveTabRegistry* registry)
    : model_(*model), favorites_(*favorites), registry_(*registry) {}

RestoredTabMatcher::~RestoredTabMatcher() = default;

SidebarNode* RestoredTabMatcher::FindEntry(const GURL& url) const {
  if (url.is_empty()) {
    return nullptr;
  }
  for (SidebarNode* root : {favorites_->pinned_root(), model_->pinned_root(),
                            model_->today_root()}) {
    if (SidebarNode* entry = FindUnloaded(*registry_, root, url)) {
      return entry;
    }
  }
  return nullptr;
}

void RestoredTabMatcher::AddUnmatched(const SidebarNode* node) {
  if (!loaded_) {
    unmatched_.push_back(node->id());
  }
}

void RestoredTabMatcher::OnLoaded() {
  loaded_ = true;
  std::vector<base::Uuid> unmatched;
  unmatched.swap(unmatched_);
  for (const base::Uuid& id : unmatched) {
    SidebarNode* node = model_->FindById(id);
    tabs::TabInterface* tab = node ? registry_->GetTab(node) : nullptr;
    SidebarNode* entry = tab ? FindEntry(node->url()) : nullptr;
    if (!entry) {
      continue;
    }
    // Unregistered first, so removing the node doesn't close the tab.
    registry_->Unregister(tab);
    registry_->Register(tab, entry);
    model_->Remove(node);
  }
}

}  // namespace compass
