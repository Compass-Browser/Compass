// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_ADBLOCK_SERVICE_H_
#define COMPASS_BROWSER_ADBLOCK_ADBLOCK_SERVICE_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/timer/timer.h"
#include "compass/browser/adblock/filter_lists.h"
#include "compass/browser/adblock/ruleset_builder.h"
#include "components/prefs/pref_change_registrar.h"

class PrefService;

namespace base {
class Clock;
class SequencedTaskRunner;
}  // namespace base

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace subresource_filter {
class RulesetService;
}  // namespace subresource_filter

namespace compass::adblock {

class FilterListUpdater;

// The ad blocker's lists and rules, for the whole browser
// (docs/design/ad-blocker.md): keeps the lists downloaded, builds their
// ruleset when the lists or their text change, and hands it to upstream's
// ruleset service, which indexes it and gives it to every renderer.
// Started after the browser starts (StartAdBlock()).
class AdBlockService {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // The lists, their state or the switch changed.
    virtual void OnFilterListsChanged() = 0;
  };

  struct ListInfo {
    ListInfo();
    ListInfo(const ListInfo&);
    ListInfo& operator=(const ListInfo&);
    ~ListInfo();

    FilterList list;
    FilterListState state;
    bool updating = false;
  };

  // How long after a change the ruleset is built, so a few changes in a row
  // build once.
  static constexpr base::TimeDelta kRebuildDelay = base::Seconds(2);
  // The first check for list updates after launch, and the ones after it
  // (a list is downloaded only once a day: FilterListUpdater::kMaxAge).
  static constexpr base::TimeDelta kFirstUpdateDelay = base::Seconds(30);
  static constexpr base::TimeDelta kUpdateInterval = base::Hours(1);

  AdBlockService(
      PrefService* local_state,
      const base::FilePath& user_data_dir,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      subresource_filter::RulesetService* ruleset_service,
      const base::Clock* clock);
  AdBlockService(const AdBlockService&) = delete;
  AdBlockService& operator=(const AdBlockService&) = delete;
  ~AdBlockService();

  // The browser's service; null before StartAdBlock() and in processes
  // without one (tests that don't start it).
  static AdBlockService* Get();

  // Builds (or finds) the current ruleset and supplies it, and schedules
  // the list updates.
  void Start();

  bool IsEnabled() const;
  void SetEnabled(bool enabled);

  std::vector<ListInfo> GetLists() const;
  void SetListEnabled(std::string_view id, bool enabled);
  // Empty on success, else why the URL can't be added.
  std::string AddList(std::string_view url);
  void RemoveList(std::string_view id);
  // Checks every enabled list with its server now.
  void UpdateNow();
  bool IsUpdating() const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Tests: stores `text` as the list's download, as if it came from its
  // server, and builds. `published` runs when the ruleset reaches the
  // renderers.
  void SetListTextForTesting(const std::string& id,
                             const std::string& text,
                             base::OnceClosure published);

 private:
  void OnListsPrefChanged();
  void OnEnabledPrefChanged();
  void ScheduleRebuild();
  void Rebuild();
  void OnRulesetBuilt(std::optional<BuiltRuleset> built);
  void OnListUpdated(const std::string& id, bool changed);
  void MaybeUpdateLists();
  void NotifyChanged();

  const raw_ptr<PrefService> local_state_;
  const base::FilePath user_data_dir_;
  const raw_ptr<subresource_filter::RulesetService> ruleset_service_;
  const raw_ptr<const base::Clock> clock_;
  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  std::unique_ptr<FilterListUpdater> updater_;
  PrefChangeRegistrar pref_change_registrar_;
  base::OneShotTimer rebuild_timer_;
  base::RepeatingTimer update_timer_;
  base::OneShotTimer first_update_timer_;
  bool building_ = false;
  bool rebuild_again_ = false;
  base::OnceClosure published_for_testing_;
  base::ObserverList<Observer> observers_;

  base::WeakPtrFactory<AdBlockService> weak_factory_{this};
};

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_ADBLOCK_SERVICE_H_
