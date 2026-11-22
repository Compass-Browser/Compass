// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_FILTER_LIST_UPDATER_H_
#define COMPASS_BROWSER_ADBLOCK_FILTER_LIST_UPDATER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"

class PrefService;

namespace base {
class Clock;
class SequencedTaskRunner;
}  // namespace base

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace compass::adblock {

// Downloads the filter lists, as iOS does (FilterListUpdater in
// FilterLists.swift): a list is asked for at most once a day unless forced,
// with If-None-Match and If-Modified-Since; a download that fails or isn't a
// filter list keeps the last good copy and records the error in the list's
// state. One list at a time.
class FilterListUpdater {
 public:
  // How long a list's last check counts.
  static constexpr base::TimeDelta kMaxAge = base::Hours(24);
  // Lists larger than this aren't downloaded.
  static constexpr int64_t kMaxListSize = 30 * 1024 * 1024;

  // `on_list_done` runs after each list is checked, with whether its text
  // changed.
  using ListDoneCallback =
      base::RepeatingCallback<void(const std::string& id, bool changed)>;

  FilterListUpdater(
      PrefService* local_state,
      const base::FilePath& user_data_dir,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      scoped_refptr<base::SequencedTaskRunner> file_task_runner,
      const base::Clock* clock,
      ListDoneCallback on_list_done);
  FilterListUpdater(const FilterListUpdater&) = delete;
  FilterListUpdater& operator=(const FilterListUpdater&) = delete;
  ~FilterListUpdater();

  // Checks every enabled list whose last check is older than kMaxAge, or
  // every enabled list when `force`. `on_done` runs once the queue is
  // empty; an update already running takes the new lists along.
  void Update(bool force, base::OnceClosure on_done);

  // Checks only the list `id`, now (a list the user just added or turned
  // on).
  void UpdateList(const std::string& id);

  bool is_updating() const { return current_.has_value(); }
  // The list being downloaded, if any.
  const std::optional<std::string>& current() const { return current_; }

 private:
  void StartNext();
  void StartRequest(const std::string& url, bool has_copy);
  void OnDownloaded(base::FilePath path);
  void OnStored(std::optional<std::string> title);
  void Finish(bool changed);

  const raw_ptr<PrefService> local_state_;
  const base::FilePath user_data_dir_;
  const scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  const raw_ptr<const base::Clock> clock_;

  std::vector<std::string> queue_;
  std::optional<std::string> current_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::string pending_etag_;
  std::string pending_last_modified_;
  const ListDoneCallback on_list_done_;
  std::vector<base::OnceClosure> on_done_;

  base::WeakPtrFactory<FilterListUpdater> weak_factory_{this};
};

// Whether a download looks like a filter list: Adblock Plus' header, or a
// comment first, as iOS checks.
bool LooksLikeFilterList(std::string_view start);

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_FILTER_LIST_UPDATER_H_
