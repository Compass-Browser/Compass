// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_SERVICE_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_SERVICE_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "compass/browser/live_folders/live_fetcher.h"
#include "compass/browser/live_folders/live_folder_store.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/live_folders/live_folder_host.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace network {
class SharedURLLoaderFactory;
}

namespace compass {

class SpaceService;

// A live folder's state that the sidebar shows besides the model's: whether
// it's refreshing, the last error, and what the plugin logged.
struct LiveFolderState {
  LiveFolderState();
  LiveFolderState(const LiveFolderState&);
  LiveFolderState& operator=(const LiveFolderState&);
  ~LiveFolderState();

  bool refreshing = false;
  std::optional<LiveError> error;
  // Errors in a row; the delay before the next try doubles with each.
  int failures = 0;
  // When it refreshes next, on its own.
  base::Time next_refresh;
  // When a refresh last started.
  base::Time last_attempt;
  // The last refresh's compass.log() lines.
  std::vector<std::string> log;
  // Whether the folder was collapsed when last seen, to tell an expand.
  bool collapsed = false;
  // Whether the folder has the plugin's name, which the first result's
  // title replaces.
  bool title_from_plugin = false;
};

// Runs a profile's live folders (docs/design/live-folders.md): the plugins
// they can use, the secrets and cookie grants users gave them, and when each
// refreshes: on a timer (the plugin's nextRefresh or its default, clamped),
// when it's expanded, when its space becomes active, and on request, with
// doubling delays after errors. Results go into the model with
// SpaceModel::ReplaceLiveItems(); the entries never sync.
class LiveFolderService : public KeyedService,
                          public SpaceList::Observer,
                          public SpaceModel::Observer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // |folder_id|'s LiveFolderState changed.
    virtual void OnLiveFolderStateChanged(const base::Uuid& folder_id) {}
  };

  // Refreshes no sooner than this after the last try when a folder is
  // expanded or its space shown.
  static constexpr base::TimeDelta kStaleAfter = base::Minutes(5);
  static constexpr base::TimeDelta kMaxInterval = base::Days(1);
  static constexpr size_t kMaxConcurrentRefreshes = 2;

  explicit LiveFolderService(Profile* profile);
  LiveFolderService(const LiveFolderService&) = delete;
  LiveFolderService& operator=(const LiveFolderService&) = delete;
  ~LiveFolderService() override;

  // Null where there's none (incognito, guest, tests).
  static LiveFolderService* Get(Profile* profile);

  const LivePluginRegistry& registry() const { return *registry_; }
  LiveFolderHost& host() { return host_; }
  LiveFolderStore& store() { return store_; }

  // Null for a folder that isn't live or has no state yet.
  const LiveFolderState* GetState(const base::Uuid& folder_id) const;

  // Refreshes |folder_id| now (unless it's refreshing).
  void Refresh(const base::Uuid& folder_id);

  // Makes a live folder for |plugin_id| in |space| and refreshes it. Returns
  // it, or an error message for the user (an invalid config, say).
  // |secrets| are stored for the plugin; empty values leave stored ones.
  base::expected<SidebarNode*, std::string> CreateLiveFolder(
      SpaceModel& space,
      SidebarNode* parent,
      size_t index,
      std::u16string title,
      const std::string& plugin_id,
      base::DictValue config,
      const std::map<std::string, std::string>& secrets,
      bool grant_cookies);

  // Changes |folder|'s config, secrets and cookie grant, and refreshes it.
  base::expected<void, std::string> EditLiveFolder(
      SidebarNode* folder,
      base::DictValue config,
      const std::map<std::string, std::string>& secrets,
      bool grant_cookies);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  void SetURLLoaderFactoryForTesting(
      scoped_refptr<network::SharedURLLoaderFactory> factory);
  // Refresh results use this as now; the timer doesn't run.
  void SetClockForTesting(base::RepeatingCallback<base::Time()> now);

  // KeyedService:
  void Shutdown() override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;
  void OnActiveSpaceChanged() override;

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeChanged(const SidebarNode& node) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;

 private:
  void OnStoreLoaded();
  void OnSpacesLoaded();

  // The live folder |id| and its space, or null.
  SidebarNode* FindFolder(const base::Uuid& id, SpaceModel** space = nullptr);
  void ForEachLiveFolder(base::FunctionRef<void(SidebarNode&)> callback);

  void StartRefresh(const base::Uuid& folder_id);
  void OnRefreshDone(base::Uuid folder_id,
                     base::Time started,
                     base::TimeDelta plugin_default,
                     base::TimeDelta plugin_min,
                     base::expected<std::string, LiveError> result,
                     std::vector<std::string> log);
  void Fail(const base::Uuid& folder_id, LiveError error);

  // Runs due refreshes and sets the timer for the next.
  void ScheduleNext();
  void RefreshIfStale(const SidebarNode& folder);

  base::Time Now() const;
  scoped_refptr<network::SharedURLLoaderFactory> GetURLLoaderFactory();
  void NotifyStateChanged(const base::Uuid& folder_id);

  const raw_ptr<Profile> profile_;
  const raw_ref<const LivePluginRegistry> registry_;
  LiveFolderHost host_;
  LiveFolderStore store_;
  base::WeakPtr<SpaceService> space_service_;
  bool spaces_loaded_ = false;
  std::map<base::Uuid, LiveFolderState> states_;
  std::map<base::Uuid, int> run_ids_;
  // Folders waiting for a free slot, in order.
  std::vector<base::Uuid> queue_;
  base::OneShotTimer timer_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_for_testing_;
  base::RepeatingCallback<base::Time()> now_for_testing_;
  base::ScopedObservation<SpaceList, SpaceList::Observer> list_observation_{
      this};
  base::ScopedMultiSourceObservation<SpaceModel, SpaceModel::Observer>
      model_observations_{this};
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<LiveFolderService> weak_factory_{this};
};

class LiveFolderServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static LiveFolderServiceFactory* GetInstance();
  static LiveFolderService* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<LiveFolderServiceFactory>;

  LiveFolderServiceFactory();
  ~LiveFolderServiceFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_FOLDER_SERVICE_H_
