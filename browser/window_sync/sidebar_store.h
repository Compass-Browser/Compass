// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_WINDOW_SYNC_SIDEBAR_STORE_H_
#define STRATA_BROWSER_WINDOW_SYNC_SIDEBAR_STORE_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/important_file_writer.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "strata/browser/sidebar/space_model.h"

namespace strata {

// Keeps a SpaceModel's pinned section in a JSON file (see
// sidebar_serializer.h). Loads it once, on a background sequence, then saves
// every change to Pinned, coalesced. Today isn't saved. Nothing is written
// before the load finishes, so an early save can't drop saved entries. See
// docs/design/sidebar.md, slice d.
class SidebarStore : public SpaceModel::Observer,
                     public base::ImportantFileWriter::DataSerializer {
 public:
  SidebarStore(const base::FilePath& path, SpaceModel* model);
  SidebarStore(const SidebarStore&) = delete;
  SidebarStore& operator=(const SidebarStore&) = delete;
  // Writes pending changes.
  ~SidebarStore() override;

  // Reads the file and adds its entries ahead of anything already pinned.
  // |done| runs once they're in the model.
  void Load(base::OnceClosure done = base::DoNothing());

  // Writes pending changes now.
  void Flush();

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeChanged(const SidebarNode& node) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;

  // base::ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

 private:
  void OnLoaded(base::OnceClosure done, std::optional<base::Value> data);
  void SaveIfPinned(const SidebarNode& node);

  const raw_ref<SpaceModel> model_;
  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  base::ImportantFileWriter writer_;
  // Set while loaded entries go into the model, which needn't be saved back.
  bool loading_ = false;
  bool loaded_ = false;
  // Pinned changed before the load finished.
  bool changed_before_load_ = false;

  base::WeakPtrFactory<SidebarStore> weak_factory_{this};
};

}  // namespace strata

#endif  // STRATA_BROWSER_WINDOW_SYNC_SIDEBAR_STORE_H_
