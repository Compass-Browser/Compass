// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/live_tab_registry.h"

#include <string>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "chrome/browser/ui/recently_audible_helper.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"

namespace compass {

// Follows a registered tab's sound: whether its contents are muted, and
// whether they played sound recently, through the contents the tab swaps to
// when it's discarded.
class LiveTabRegistry::AudioWatch : public content::WebContentsObserver {
 public:
  AudioWatch(LiveTabRegistry* registry, tabs::TabInterface* tab)
      : registry_(registry), tab_(tab) {
    discard_subscription_ = tab->RegisterWillDiscardContents(
        base::BindRepeating(&AudioWatch::OnWillDiscardContents,
                            base::Unretained(this)));
    Watch(tab->GetContents());
    state_ = ComputeState();
  }
  AudioWatch(const AudioWatch&) = delete;
  AudioWatch& operator=(const AudioWatch&) = delete;
  ~AudioWatch() override = default;

  AudioState state() const { return state_; }

 private:
  void Watch(content::WebContents* contents) {
    Observe(contents);
    audible_subscription_ = {};
    if (!contents) {
      return;
    }
    // Every tab has one (a tab feature since Chromium 157); contents that
    // aren't in a tab don't.
    RecentlyAudibleHelper* helper =
        RecentlyAudibleHelper::FromWebContents(contents);
    if (!helper) {
      return;
    }
    audible_subscription_ = helper->RegisterRecentlyAudibleChangedCallback(
        base::BindRepeating([](AudioWatch* watch, bool) { watch->Update(); },
                            base::Unretained(this)));
  }

  void OnWillDiscardContents(tabs::TabInterface* tab,
                             content::WebContents* old_contents,
                             content::WebContents* new_contents) {
    Watch(new_contents);
    if (new_contents != old_contents) {
      registry_->ApplyNodeMuted(tab_, new_contents);
    }
    Update();
  }

  // content::WebContentsObserver:
  void DidUpdateAudioMutingState(bool muted) override {
    if (!registry_->applying_muted_) {
      registry_->OnMutingChanged(tab_, muted);
    }
    Update();
  }

  AudioState ComputeState() const {
    content::WebContents* contents = web_contents();
    if (!contents) {
      return AudioState::kNone;
    }
    if (contents->IsAudioMuted()) {
      return AudioState::kMuted;
    }
    RecentlyAudibleHelper* helper =
        RecentlyAudibleHelper::FromWebContents(contents);
    return helper && helper->WasRecentlyAudible() ? AudioState::kAudible
                                                  : AudioState::kNone;
  }

  void Update() {
    const AudioState state = ComputeState();
    if (state == state_) {
      return;
    }
    state_ = state;
    registry_->OnAudioChanged(tab_);
  }

  const raw_ptr<LiveTabRegistry> registry_;
  const raw_ptr<tabs::TabInterface> tab_;
  AudioState state_ = AudioState::kNone;
  base::CallbackListSubscription discard_subscription_;
  base::CallbackListSubscription audible_subscription_;
};

LiveTabRegistry::LiveTabRegistry() = default;

LiveTabRegistry::~LiveTabRegistry() = default;

void LiveTabRegistry::Register(tabs::TabInterface* tab, SidebarNode* node) {
  CHECK(tab);
  CHECK(node);
  CHECK(!GetTab(node));
  node_for_tab_[tab->GetHandle()] = node;
  tab_for_node_[node] = tab->GetHandle();
  // Before anyone hears of it, so observers see it muted.
  ApplyNodeMuted(tab, tab->GetContents());
  auto& watch = audio_[tab->GetHandle()];
  if (!watch) {
    watch = std::make_unique<AudioWatch>(this, tab);
  }
  for (auto& observer : observers_) {
    observer.OnNodeLiveChanged(*node, true);
  }
  // A tab muted before it had this node mutes it.
  if (!node->muted() && tab->GetContents() &&
      tab->GetContents()->IsAudioMuted()) {
    for (auto& observer : observers_) {
      observer.OnNodeMutingChanged(*node, true);
    }
  }
  // A tab that moved here, or that played before it had a node.
  if (watch->state() != AudioState::kNone) {
    for (auto& observer : observers_) {
      observer.OnNodeAudioChanged(*node);
    }
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
  auto watch = audio_.find(tab->GetHandle());
  const bool had_audio =
      watch != audio_.end() && watch->second->state() != AudioState::kNone;
  if (watch != audio_.end()) {
    audio_.erase(watch);
  }
  for (auto& observer : observers_) {
    observer.OnNodeLiveChanged(*node, false);
  }
  if (had_audio) {
    for (auto& observer : observers_) {
      observer.OnNodeAudioChanged(*node);
    }
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

LiveTabRegistry::AudioState LiveTabRegistry::GetAudioState(
    const SidebarNode* node) const {
  tabs::TabInterface* tab = GetTab(node);
  if (!tab) {
    return AudioState::kNone;
  }
  auto it = audio_.find(tab->GetHandle());
  return it == audio_.end() ? AudioState::kNone : it->second->state();
}

void LiveTabRegistry::OnAudioChanged(tabs::TabInterface* tab) {
  if (SidebarNode* node = GetNode(tab)) {
    for (auto& observer : observers_) {
      observer.OnNodeAudioChanged(*node);
    }
  }
}

void LiveTabRegistry::OnMutingChanged(tabs::TabInterface* tab, bool muted) {
  if (SidebarNode* node = GetNode(tab)) {
    for (auto& observer : observers_) {
      observer.OnNodeMutingChanged(*node, muted);
    }
  }
}

void LiveTabRegistry::ApplyNodeMuted(tabs::TabInterface* tab,
                                     content::WebContents* contents) {
  SidebarNode* node = GetNode(tab);
  if (!node || !node->muted() || !contents || contents->IsAudioMuted()) {
    return;
  }
  base::AutoReset<bool> applying(&applying_muted_, true);
  SetTabAudioMuted(contents, true, TabMutedReason::kAudioIndicator,
                   std::string());
}

void LiveTabRegistry::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void LiveTabRegistry::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

}  // namespace compass
