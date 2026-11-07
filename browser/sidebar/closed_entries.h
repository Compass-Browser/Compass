// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_CLOSED_ENTRIES_H_
#define COMPASS_BROWSER_SIDEBAR_CLOSED_ENTRIES_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/functional/function_ref.h"
#include "base/memory/raw_ref.h"
#include "base/uuid.h"
#include "base/values.h"
#include "url/gurl.h"

namespace compass {

class SidebarNode;
class SpaceModel;

// A profile's sidebar entries the user closed, newest last, so that Cmd+Z
// puts them back, and Cmd+Shift+Z closes them again. Entries are kept as
// data, the way archived ones are, so unloaded entries come back as well as
// open ones; an open tab's history is kept by upstream's TabRestoreService,
// and only its entry's ID is kept here. In memory only. See
// docs/design/sidebar.md, "Undo closing".
class ClosedEntries {
 public:
  // At most this many steps are kept; older ones are dropped.
  static constexpr size_t kMaxSteps = 25;

  // One thing that was closed.
  struct Entry {
    Entry();
    Entry(Entry&&);
    Entry& operator=(Entry&&);
    ~Entry();

    // The entry's node: removed with its subtree, or a pinned tab or favorite
    // that stayed, unloaded.
    base::Uuid id;
    bool favorites = false;

    // Removed: the subtree (SerializeEntry()), the IDs of its nodes, and its
    // place: its parent, the sibling before it (none if it was first) and
    // its index. A pane also keeps the split's other pane, since the split
    // ended when it left. Empty for a tab that stayed.
    std::optional<base::DictValue> node;
    std::vector<base::Uuid> ids;
    base::Uuid parent_id;
    base::Uuid previous_id;
    base::Uuid split_sibling_id;
    size_t index = 0;
    bool in_today = false;

    // A tab that stayed: the page it showed before it went back to its
    // pinned URL.
    GURL url;
    std::u16string title;
  };

  // What one user action closed: a tab, a split, a folder, Clear Today.
  struct Step {
    Step();
    Step(Step&&);
    Step& operator=(Step&&);
    ~Step();

    // Whether |id| is one of its nodes.
    bool Contains(const base::Uuid& id) const;

    // The space its entries were in; invalid if they were all favorites.
    base::Uuid space_id;
    std::vector<Entry> entries;
    // The tab that comes back open: the first its window showed (closing it
    // shows the next, which the user wasn't looking at), else the first
    // that was open. Invalid if none was open.
    base::Uuid reopen_id;
    bool reopen_was_shown = false;
    // TabRestoreService entries (SessionID values) with the history of the
    // tabs that were open, by node.
    base::flat_map<base::Uuid, int32_t> history;
  };

  // Everything recorded while one is alive is one step. They nest. A step
  // recorded while |keep_redo| is set (a redo) leaves the steps to redo;
  // any other clears them.
  class ScopedStep {
   public:
    explicit ScopedStep(ClosedEntries& closed, bool keep_redo = false);
    ScopedStep(const ScopedStep&) = delete;
    ScopedStep& operator=(const ScopedStep&) = delete;
    ~ScopedStep();

   private:
    const raw_ref<ClosedEntries> closed_;
  };

  ClosedEntries();
  ClosedEntries(const ClosedEntries&) = delete;
  ClosedEntries& operator=(const ClosedEntries&) = delete;
  ~ClosedEntries();

  // |entry|, a tab, split or folder of |model|, is about to be removed with
  // its subtree. Call it before anything is removed: a step's entries come
  // back in the order they were recorded. A Today folder that will close as
  // it empties is recorded in its place, with the entry.
  void RecordRemoval(const SpaceModel& model, const SidebarNode& entry);
  // The live tab of |tab|, a pinned tab or favorite of |model|, is closing;
  // the node stays, unloaded.
  void RecordUnload(const SpaceModel& model, const SidebarNode& tab);

  // The live tab of |tab| is about to close for a step already recorded, by
  // the sidebar (not where the tab closes, which then records nothing).
  void ExpectClose(const base::Uuid& tab);
  // Whether the closing tab of |tab| was expected; true once.
  bool TakeExpectedClose(const base::Uuid& tab);

  // In the latest step that has it, |tab| was open (|shown| in its window),
  // and its history is TabRestoreService entry |history|, if it has one.
  void SetOpen(const base::Uuid& tab, bool shown);
  void SetHistory(const base::Uuid& tab, int32_t history);

  bool CanUndo() const { return !undo_.empty(); }
  bool CanRedo() const { return !redo_.empty(); }

  // Takes the latest step to undo; the caller puts it back in its space
  // (Restore()), then hands it to PushRedo().
  std::optional<Step> TakeUndo();
  void PushRedo(Step step);
  // Takes the latest step to redo; closing its entries again records a new
  // step (in a ScopedStep with |keep_redo|).
  std::optional<Step> TakeRedo();

  // Puts the entries of |step| back into |model| (its space) or |favorites|,
  // unloaded; a tab that stayed goes back to the page it showed, unless
  // |is_open| says it's open again. Entries still there are left alone.
  // Returns the tab that should open, if it's there.
  static SidebarNode* Restore(
      const Step& step,
      SpaceModel& model,
      SpaceModel& favorites,
      base::FunctionRef<bool(const SidebarNode&)> is_open);

  // The steps, oldest first, for tests.
  const std::vector<Step>& undo_steps_for_testing() const { return undo_; }

 private:
  // The step being recorded: the open ScopedStep's, or a new one.
  Step& CurrentStep(const SpaceModel& model);
  // Ends a step outside any ScopedStep.
  void EndStep();
  // The latest step that has |id|, or null.
  Step* FindStep(const base::Uuid& id);

  std::vector<Step> undo_;
  std::vector<Step> redo_;
  base::flat_set<base::Uuid> expected_closes_;
  // ScopedSteps alive, and whether the open step has been started.
  int open_steps_ = 0;
  bool keep_redo_ = false;
  bool step_started_ = false;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_CLOSED_ENTRIES_H_
