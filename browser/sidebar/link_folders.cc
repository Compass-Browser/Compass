// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/link_folders.h"

#include "base/strings/utf_string_conversions.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"

namespace compass {

std::u16string GetLinkFolderTitle(const SidebarNode& opener) {
  const SidebarNode* tab = opener.type() == SidebarNode::Type::kSplit &&
                                   !opener.children().empty()
                               ? opener.children()[0].get()
                               : &opener;
  if (!tab->display_title().empty()) {
    return tab->display_title();
  }
  return base::UTF8ToUTF16(tab->url().host());
}

LinkFolders::LinkFolders() = default;
LinkFolders::~LinkFolders() = default;

SidebarNode* LinkFolders::AddLink(SpaceModel& space,
                                  const SidebarNode& opener,
                                  const GURL& url,
                                  const std::u16string& title) {
  if (opener.type() != SidebarNode::Type::kTab || !opener.parent()) {
    return nullptr;
  }
  // A split's pane opens next to its split.
  SidebarNode* entry = opener.parent()->type() == SidebarNode::Type::kSplit
                           ? opener.parent()
                           : const_cast<SidebarNode*>(&opener);
  SidebarNode* today = space.today_root();
  SidebarNode* folder = nullptr;
  size_t index = 0;
  if (today != entry && today->Contains(entry)) {
    folder = entry->parent();
    if (folder->type() != SidebarNode::Type::kFolder ||
        folder->is_live_folder()) {
      if (folder != today) {
        return nullptr;
      }
      // The opener's own folder takes its place.
      folder = space.AddFolder(today, today->GetIndexOf(entry),
                               GetLinkFolderTitle(*entry));
      // Claude may name it (docs/design/ai-sidebar.md); only this device
      // asks.
      space.MarkLinkFolder(folder);
      space.Move(entry, folder, 0);
    }
    index = folder->GetIndexOf(entry) + 1;
  } else {
    // Pinned entries and favorites stay put; their links gather in a folder
    // at the top of Today.
    auto it = folders_.find(opener.id());
    folder = it != folders_.end() ? space.FindById(it->second) : nullptr;
    if (!folder || folder == today || !today->Contains(folder) ||
        folder->type() != SidebarNode::Type::kFolder ||
        folder->is_live_folder()) {
      folder = space.AddFolder(today, 0, GetLinkFolderTitle(*entry));
      space.MarkLinkFolder(folder);
      folders_[opener.id()] = folder->id();
    }
    index = folder->children().size();
  }
  // After the link the opener opened before, if it's still here.
  if (auto it = last_link_.find(opener.id()); it != last_link_.end()) {
    for (size_t i = 0; i < folder->children().size(); ++i) {
      if (folder->children()[i]->id() == it->second) {
        index = i + 1;
        break;
      }
    }
  }
  SidebarNode* link = space.AddTab(folder, index, url, title);
  if (link) {
    last_link_[opener.id()] = link->id();
  }
  return link;
}

}  // namespace compass
