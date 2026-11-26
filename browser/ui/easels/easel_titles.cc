// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_titles.h"

#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass::easels {

namespace {

bool IsEntryOf(const SidebarNode& node, std::string_view easel_uuid) {
  return EaselUuidFromUrl(node.url().spec()) == easel_uuid ||
         (node.saved_url().is_valid() &&
          EaselUuidFromUrl(node.saved_url().spec()) == easel_uuid);
}

// The tab entries of |easel_uuid| under |node|.
void Collect(SidebarNode* node,
             std::string_view easel_uuid,
             std::vector<SidebarNode*>& out) {
  if (!node) {
    return;
  }
  if (node->type() == SidebarNode::Type::kTab) {
    if (IsEntryOf(*node, easel_uuid)) {
      out.push_back(node);
    }
    return;
  }
  for (const auto& child : node->children()) {
    Collect(child.get(), easel_uuid, out);
  }
}

}  // namespace

std::string NewEaselTitle() {
  return l10n_util::GetStringUTF8(IDS_COMPASS_EASEL_UNTITLED);
}

std::u16string EaselTabTitle(const sync_pb::CompassEasel& easel) {
  return base::UTF8ToUTF16(easel.title().empty() ? NewEaselTitle()
                                                 : easel.title());
}

size_t ApplyEaselTitle(SpaceModel& space,
                       std::string_view easel_uuid,
                       const std::u16string& title) {
  if (easel_uuid.empty()) {
    return 0;
  }
  // Gathered first: the changes notify observers, which may look at the
  // tree.
  std::vector<SidebarNode*> entries;
  Collect(space.pinned_root(), easel_uuid, entries);
  Collect(space.today_root(), easel_uuid, entries);
  size_t changed = 0;
  for (SidebarNode* node : entries) {
    bool any = false;
    // Only an entry that shows the board now: one that went on to another
    // page keeps that page's title.
    if (EaselUuidFromUrl(node->url().spec()) == easel_uuid &&
        node->title() != title) {
      space.SetTitle(node, title);
      any = true;
    }
    if (node->saved_url().is_valid() &&
        EaselUuidFromUrl(node->saved_url().spec()) == easel_uuid &&
        node->saved_title() != title) {
      space.SetSavedUrl(node, node->saved_url(), title);
      any = true;
    }
    changed += any;
  }
  return changed;
}

EaselTitleFollower::EaselTitleFollower(EaselStore* store, ModelsGetter models)
    : store_(store->GetWeakPtr()), models_(std::move(models)) {
  store->AddObserver(this);
  if (store->loaded()) {
    OnEaselChanged(std::string());
  }
}

EaselTitleFollower::~EaselTitleFollower() {
  if (store_) {
    store_->RemoveObserver(this);
  }
}

void EaselTitleFollower::OnEaselChanged(const std::string& easel_uuid) {
  if (!easel_uuid.empty()) {
    if (const sync_pb::CompassEasel* easel = store_->FindEasel(easel_uuid)) {
      Follow(*easel);
    } else {
      // Deleted: its entries keep the last title, as a tab that can't open.
      titles_.erase(easel_uuid);
    }
    return;
  }
  // Loading or sync: any board may have changed.
  for (const sync_pb::CompassEasel* easel : store_->Easels()) {
    Follow(*easel);
  }
}

void EaselTitleFollower::Follow(const sync_pb::CompassEasel& easel) {
  const std::u16string title = EaselTabTitle(easel);
  auto it = titles_.find(easel.uuid());
  if (it != titles_.end() && it->second == title) {
    return;
  }
  titles_[easel.uuid()] = title;
  for (SpaceModel* space : models_.Run()) {
    ApplyEaselTitle(*space, easel.uuid(), title);
  }
}

}  // namespace compass::easels
