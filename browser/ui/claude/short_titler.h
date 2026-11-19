// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_SHORT_TITLER_H_
#define COMPASS_BROWSER_UI_CLAUDE_SHORT_TITLER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "compass/browser/claude/claude_quick.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/claude/sidebar_spaces.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"

class PrefService;
class Profile;

namespace compass {


// Shorter Tab Titles (docs/design/ai-sidebar.md, "Shorter tab titles"):
// with "Shorter Tab Titles" on (prefs::kShortTabTitles, and Claude set up),
// tabs whose titles are long or end with a site's name get a short title
// from Claude (SpaceModel::SetShortTitle()), which the sidebar's row shows.
// Display only: never saved or synced; a user's own name wins. Titles that
// changed in the last kBatchDelay go in one background request (at most
// kMaxBatch, sooner when that many wait); answers are kept per (host,
// title) for the session, so a title seen again costs nothing. Today's and
// Pinned's tabs without a custom title; not favorites, whose tiles show no
// title. Never off the record: incognito's spaces are never followed.
class ShortTitler : public KeyedService,
                    public SpaceList::Observer,
                    public SpaceModel::Observer {
 public:
  static constexpr base::TimeDelta kBatchDelay = base::Seconds(10);
  static constexpr size_t kMaxBatch = 20;
  // The answers kept; past that the cache starts over.
  static constexpr size_t kMaxCache = 2000;

  using GetLocale = base::RepeatingCallback<std::string()>;

  // |prefs| has the profile's Claude prefs; |quick| asks Claude.
  ShortTitler(PrefService* prefs,
              claude::ClaudeQuick* quick,
              GetLocale get_locale);
  ShortTitler(const ShortTitler&) = delete;
  ShortTitler& operator=(const ShortTitler&) = delete;
  ~ShortTitler() override;

  // Null off the record and for guests.
  static ShortTitler* Get(Profile* profile);

  // The user message for |tabs| (title and host each): a line each,
  // "<n>. <title> (<host>)", with n from 1.
  static std::string DescribeTitles(
      const std::vector<std::pair<std::u16string, std::string>>& tabs);

  // Whether short titles are wanted at all ("Shorter Tab Titles" on, and
  // Claude set up).
  bool IsEnabled() const;
  // Whether |tab| of |space| should show a short title: a tab of a space
  // (not Favorites) without a custom title, whose title NeedsShortTitle().
  static bool IsEligible(const SpaceModel& space, const SidebarNode& tab);

  // Follows |space|'s tabs (the profile's spaces, once they load). Public
  // for tests, which follow a space of their own.
  void Observe(SpaceModel& space);
  // Follows the profile's spaces, when they're loaded.
  void ObserveSpaces(SidebarSpaces* spaces);

  base::WeakPtr<ShortTitler> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // Waits |delay| instead of kBatchDelay before a request.
  void SetBatchDelayForTesting(base::TimeDelta delay) {
    batch_delay_ = delay;
  }
  // Whether it follows the profile's spaces yet (they load after it's made).
  bool IsObservingForTesting() const { return list_observation_.IsObserving(); }
  // Tabs waiting for the next request.
  size_t pending_for_testing() const { return pending_.size(); }
  // Whether the next request waits for its timer.
  bool IsWaitingForTesting() const { return timer_.IsRunning(); }
  // Sends what waits now, as the timer would.
  void FlushForTesting() { SendBatch(); }

  // KeyedService:
  void Shutdown() override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeChanged(const SidebarNode& node) override;

 private:
  // A title as it's asked and kept: the tab's host and its title.
  using Key = std::pair<std::string, std::u16string>;

  static Key KeyOf(const SidebarNode& tab);
  void OnSpacesLoaded();
  // The followed space |node| is in, or null.
  SpaceModel* SpaceOf(const SidebarNode& node);
  // Every tab under |node| (or |node| itself) gets considered.
  void ConsiderTree(SpaceModel& space, const SidebarNode& node);
  // Gives |tab| its short title from the cache, or queues it for Claude.
  void Consider(SpaceModel& space, const SidebarNode& tab);
  void ConsiderAll();
  // Gives the tab |tab_id| its cached short title, after the notification
  // Consider() ran from.
  void ApplyCachedToTab(const base::Uuid& tab_id);
  // Sends what waits right after the current task.
  void SendSoon();
  void SendBatch();
  void OnReply(std::vector<Key> keys, claude::ClaudeQuick::Result result);
  // Gives every eligible tab whose title is |key|'s its cached short title.
  void ApplyCached(const Key& key);
  // The switch or the key changed.
  void OnSettingsChanged();

  const raw_ptr<PrefService> prefs_;
  const raw_ptr<claude::ClaudeQuick> quick_;
  const GetLocale get_locale_;
  base::WeakPtr<SidebarSpaces> space_service_;
  // The spaces followed.
  std::vector<raw_ptr<SpaceModel>> spaces_;
  // Tabs waiting for the next request, by id, and the space each is in; in
  // the order they came.
  std::vector<base::Uuid> pending_order_;
  std::map<base::Uuid, raw_ptr<SpaceModel>> pending_;
  base::OneShotTimer timer_;
  base::TimeDelta batch_delay_ = kBatchDelay;
  // A SendBatch() is posted (SendSoon()).
  bool send_posted_ = false;
  // Claude's answers by (host, title); empty when it gave none.
  std::map<Key, std::u16string> cache_;
  // Titles asked for, whose answer hasn't come.
  std::set<Key> asking_;
  bool enabled_ = false;
  PrefChangeRegistrar registrar_;
  base::ScopedObservation<SpaceList, SpaceList::Observer> list_observation_{
      this};
  base::ScopedMultiSourceObservation<SpaceModel, SpaceModel::Observer>
      model_observations_{this};
  base::WeakPtrFactory<ShortTitler> weak_factory_{this};
};

class ShortTitlerFactory : public ProfileKeyedServiceFactory {
 public:
  static ShortTitlerFactory* GetInstance();
  static ShortTitler* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<ShortTitlerFactory>;

  ShortTitlerFactory();
  ~ShortTitlerFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_CLAUDE_SHORT_TITLER_H_
