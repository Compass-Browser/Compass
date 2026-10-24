// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_RESTORED_TAB_MATCHER_H_
#define STRATA_BROWSER_WINDOW_SYNC_RESTORED_TAB_MATCHER_H_

#include <vector>

#include "base/memory/raw_ref.h"
#include "base/uuid.h"

class GURL;

namespace strata {

class LiveTabRegistry;
class SidebarNode;
class SpaceModel;

// Ties tabs that Chrome restores (session restore, reopening a closed tab) to
// the saved sidebar entries they were open on. The sidebar file brings entries
// back unloaded, and Chrome brings back tabs with their history; a restored
// tab takes over the first unloaded entry with its URL, pinned first, instead
// of becoming a new Today entry. The file may load after Chrome has started
// restoring, so tabs restored before that are matched once it's in. See
// docs/design/sidebar.md, "Today persistence".
class RestoredTabMatcher {
 public:
  RestoredTabMatcher(SpaceModel* model, LiveTabRegistry* registry);
  RestoredTabMatcher(const RestoredTabMatcher&) = delete;
  RestoredTabMatcher& operator=(const RestoredTabMatcher&) = delete;
  ~RestoredTabMatcher();

  // The unloaded entry a tab restored at |url| should take over, or null.
  SidebarNode* FindEntry(const GURL& url) const;

  // |node| was made for a restored tab that FindEntry() had no entry for.
  // Before the load, the entry may still come.
  void AddUnmatched(const SidebarNode* node);

  // The saved entries are in: hands each unmatched tab to its entry, and
  // drops the node it had.
  void OnLoaded();

 private:
  const raw_ref<SpaceModel> model_;
  const raw_ref<LiveTabRegistry> registry_;
  bool loaded_ = false;
  std::vector<base::Uuid> unmatched_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_RESTORED_TAB_MATCHER_H_
