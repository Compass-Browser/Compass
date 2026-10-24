// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/sidebar_store.h"

#include <utility>

#include "base/auto_reset.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "strata/browser/sidebar/sidebar_serializer.h"

namespace strata {

namespace {

// Short, so little is lost if the session ends: logging out of macOS exits
// without closing windows or destroying the profile. Bookmarks use 2.5 s.
// Browsing changes Today's titles and URLs often, but the file is small.
constexpr base::TimeDelta kCommitInterval = base::Seconds(1);

scoped_refptr<base::SequencedTaskRunner> CreateFileTaskRunner() {
  return base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
}

std::optional<base::Value> ReadFile(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
  return base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
}

}  // namespace

SidebarStore::SidebarStore(const base::FilePath& path, SpaceList* list)
    : list_(*list),
      file_task_runner_(CreateFileTaskRunner()),
      writer_(path, file_task_runner_, kCommitInterval, "StrataSidebar") {
  list_->AddObserver(this);
  for (const auto& space : list_->spaces()) {
    space->AddObserver(this);
  }
  list_->favorites().AddObserver(this);
}

SidebarStore::~SidebarStore() {
  list_->favorites().RemoveObserver(this);
  for (const auto& space : list_->spaces()) {
    space->RemoveObserver(this);
  }
  list_->RemoveObserver(this);
  Flush();
}

void SidebarStore::Load(base::OnceClosure done) {
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadFile, writer_.path()),
      base::BindOnce(&SidebarStore::OnLoaded, weak_factory_.GetWeakPtr(),
                     std::move(done)));
}

void SidebarStore::Flush() {
  if (writer_.HasPendingWrite()) {
    writer_.DoScheduledWrite();
  }
}

void SidebarStore::OnSpaceAdded(SpaceModel& space) {
  space.AddObserver(this);
  ScheduleSave();
}

void SidebarStore::OnSpaceWillBeRemoved(SpaceModel& space) {
  space.RemoveObserver(this);
  // The write happens later, without it.
  ScheduleSave();
}

void SidebarStore::OnActiveSpaceChanged() {
  ScheduleSave();
}

void SidebarStore::OnNodeAdded(const SidebarNode& node) {
  ScheduleSave();
}

void SidebarStore::OnNodeMoved(const SidebarNode& node,
                               const SidebarNode& old_parent,
                               size_t old_index) {
  ScheduleSave();
}

void SidebarStore::OnNodeChanged(const SidebarNode& node) {
  ScheduleSave();
}

void SidebarStore::OnNodeWillBeRemoved(const SidebarNode& node) {
  ScheduleSave();
}

void SidebarStore::OnNodeWillMoveToSpace(const SidebarNode& node) {
  ScheduleSave();
}

void SidebarStore::OnNodeLastActiveChanged(const SidebarNode& node) {
  ScheduleSave();
}

void SidebarStore::OnSpaceChanged(const SpaceModel& space) {
  ScheduleSave();
}

void SidebarStore::OnArchiveChanged(const SpaceModel& space) {
  ScheduleSave();
}

std::optional<std::string> SidebarStore::SerializeData() {
  return base::WriteJsonWithOptions(SerializeSpaceList(*list_),
                                    base::JSONWriter::OPTIONS_PRETTY_PRINT);
}

void SidebarStore::OnLoaded(base::OnceClosure done,
                            std::optional<base::Value> data) {
  if (data && data->is_dict()) {
    base::AutoReset<bool> loading(&loading_, true);
    DeserializeSpaceList(data->GetDict(), *list_);
  }
  loaded_ = true;
  if (changed_before_load_) {
    writer_.ScheduleWrite(this);
  }
  std::move(done).Run();
}

void SidebarStore::ScheduleSave() {
  if (loading_) {
    return;
  }
  if (loaded_) {
    writer_.ScheduleWrite(this);
  } else {
    changed_before_load_ = true;
  }
}

}  // namespace strata
