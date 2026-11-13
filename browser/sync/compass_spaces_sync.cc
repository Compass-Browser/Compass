// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_spaces_sync.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "compass/browser/sync/favicon_png.h"
#include "components/favicon/core/favicon_service.h"
#include "components/favicon_base/favicon_types.h"
#include "components/history/core/browser/history_service.h"
#include "components/keyed_service/core/service_access_type.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/image/image.h"
#include "chrome/browser/sync/data_type_store_service_factory.h"
#include "chrome/common/channel_info.h"
#include "compass/browser/claude/claude_memory.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "compass/browser/sync/compass_sync_service.h"
#include "compass/browser/sync/compass_sync_service_factory.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/components/sync/controllers_hook.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/report_unrecoverable_error.h"
#include "components/sync/model/client_tag_based_data_type_processor.h"
#include "components/sync/model/data_type_store_service.h"
#include "components/sync/model/proxy_data_type_controller_delegate.h"
#include "components/sync/service/data_type_controller.h"

namespace compass::sync {

namespace {

// After sync starts, the icons of the pages open then are offered once the
// tabs a startup restores have had time to load.
constexpr base::TimeDelta kFaviconSweepDelay = base::Seconds(30);

// The path of the icon URL a synced icon is stored under: one per version,
// since FaviconService keeps the bitmaps of an icon URL it knows. Icons
// under it came through sync and never go back up.
constexpr char kSyncedFaviconPath[] = "/compass-synced-favicon/";

// Each listed space's directory position as its profile's bridge last synced
// it, browser-wide, so every bridge places its spaces among the others' and
// the switcher can sort spaces of several accounts (slice 5).
std::map<std::string, syncer::UniquePosition>& DirectoryPositions() {
  static base::NoDestructor<std::map<std::string, syncer::UniquePosition>>
      positions;
  return *positions;
}

// Set while the switcher is being sorted by those positions.
bool g_sorting_directory = false;

// Sorts the switcher's spaces that have a synced position by it; the others
// keep their slots.
void SortDirectoryByPositions() {
  SpaceDirectory* directory = SpaceDirectory::GetIfExists();
  if (!directory || g_sorting_directory) {
    return;
  }
  base::AutoReset<bool> sorting(&g_sorting_directory, true);
  const std::vector<SpaceDirectory::Entry> spaces = directory->GetSpaces();
  std::vector<SpaceModel*> wanted;
  std::vector<size_t> slots;
  std::vector<std::pair<syncer::UniquePosition, SpaceModel*>> positioned;
  for (size_t i = 0; i < spaces.size(); ++i) {
    SpaceModel* space = spaces[i].space;
    wanted.push_back(space);
    auto it = DirectoryPositions().find(space->id().AsLowercaseString());
    if (it != DirectoryPositions().end()) {
      slots.push_back(i);
      positioned.emplace_back(it->second, space);
    }
  }
  std::ranges::stable_sort(positioned, [](const auto& a, const auto& b) {
    return a.first.LessThan(b.first);
  });
  for (size_t k = 0; k < slots.size(); ++k) {
    wanted[slots[k]] = positioned[k].second;
  }
  for (size_t i = 0; i < wanted.size(); ++i) {
    const std::vector<SpaceDirectory::Entry> now = directory->GetSpaces();
    if (i < now.size() && now[i].space != wanted[i]) {
      // It's further down: the slots before i hold wanted[0..i).
      directory->MoveSpace(*wanted[i], i);
    }
  }
}

// The bridge, and what it needs from windows: deleting spaces and moving
// entries with their live tabs (window_features.h), and the switcher's order
// of the profile's spaces, which the bridge reads from (and writes to) the
// profile's SpaceList. Owned by the profile's SpaceService.
class SpacesSyncHolder : public SpaceService::Attachment,
                         public CompassSpacesSyncBridge::Delegate,
                         public SpaceList::Observer,
                         public SpaceDirectory::Observer,
                         public claude::ClaudeMemory::Observer {
 public:
  SpacesSyncHolder(Profile* profile, SpaceService* service)
      : profile_(profile), service_(service) {
    bridge_ = std::make_unique<CompassSpacesSyncBridge>(
        std::make_unique<syncer::ClientTagBasedDataTypeProcessor>(
            syncer::COMPASS_SPACES,
            base::BindRepeating(&syncer::ReportUnrecoverableError,
                                chrome::GetChannel())),
        DataTypeStoreServiceFactory::GetForProfile(profile)->GetStoreFactory(),
        &service->space_list(), this);
    service->space_list().AddObserver(this);
    if (SpaceDirectory* directory = SpaceDirectory::GetIfExists()) {
      directory->AddObserver(this);
    }
    service->RunWhenLoaded(base::BindOnce(&SpacesSyncHolder::OnLoaded,
                                          weak_factory_.GetWeakPtr()));
    // Claude's memory syncs with the spaces (docs/design/claude.md,
    // "Memory"); it's a service of its own, which may go first.
    if (claude::ClaudeMemory* memory = claude::ClaudeMemory::Get(profile)) {
      memory_ = memory->GetWeakPtr();
      memory->AddObserver(this);
    }
  }
  SpacesSyncHolder(const SpacesSyncHolder&) = delete;
  SpacesSyncHolder& operator=(const SpacesSyncHolder&) = delete;

  ~SpacesSyncHolder() override {
    // The directory goes with the profile manager, maybe first.
    if (SpaceDirectory* directory = SpaceDirectory::GetIfExists()) {
      directory->RemoveObserver(this);
    }
    service_->space_list().RemoveObserver(this);
    if (memory_) {
      memory_->RemoveObserver(this);
    }
    bridge_.reset();
  }

  CompassSpacesSyncBridge& bridge() { return *bridge_; }

  // CompassSpacesSyncBridge::Delegate:
  std::optional<std::vector<sync_pb::CompassMemory>> GetLocalMemories()
      override {
    if (!memory_ || !memory_->loaded()) {
      return std::nullopt;
    }
    std::vector<sync_pb::CompassMemory> memories;
    for (const claude::MemoryEntry& entry : memory_->entries()) {
      sync_pb::CompassMemory& memory = memories.emplace_back();
      memory.set_uuid(entry.id);
      memory.set_text(entry.text);
      memory.set_create_time(
          entry.created.ToDeltaSinceWindowsEpoch().InMicroseconds());
      memory.set_update_time(
          entry.updated.ToDeltaSinceWindowsEpoch().InMicroseconds());
    }
    return memories;
  }

  void ApplyMemories(const std::vector<sync_pb::CompassMemory>& changed,
                     const std::set<std::string>& deleted) override {
    if (!memory_) {
      return;
    }
    std::vector<claude::MemoryEntry> entries;
    for (const sync_pb::CompassMemory& memory : changed) {
      claude::MemoryEntry& entry = entries.emplace_back();
      entry.id = memory.uuid();
      entry.text = memory.text();
      entry.created = base::Time::FromDeltaSinceWindowsEpoch(
          base::Microseconds(memory.create_time()));
      entry.updated = base::Time::FromDeltaSinceWindowsEpoch(
          base::Microseconds(memory.update_time()));
    }
    memory_->ApplyRemote(entries, deleted);
  }

  // claude::ClaudeMemory::Observer:
  void OnMemoriesChanged() override { bridge_->NotifyLocalChange(); }

  void DeleteSpace(SpaceModel& space) override {
    ::compass::DeleteSpace(profile_, space);
  }
  void MoveEntry(SidebarNode* node,
                 SpaceModel& target,
                 SidebarNode* parent,
                 size_t index) override {
    ::compass::MoveEntry(profile_, node, target, parent, index);
  }
  bool IsShownInFocusedWindow(const SidebarNode& entry) override {
    return ::compass::IsEntryShownInFocusedWindow(profile_, entry);
  }
  bool IsInUse(const SidebarNode& entry) override {
    return ::compass::IsEntryInUse(profile_, entry);
  }
  void TransferLiveTab(const SidebarNode& from, SidebarNode& to) override {
    LiveTabRegistry& registry = service_->registry();
    tabs::TabInterface* tab = registry.GetTab(&from);
    if (!tab || registry.GetTab(&to)) {
      return;
    }
    // Unregistered first, so removing |from| doesn't close it.
    registry.Unregister(tab);
    registry.Register(tab, &to);
  }
  std::optional<std::vector<sync_pb::CompassProfile>> GetLocalProfiles()
      override {
    CompassSyncService* service =
        CompassSyncServiceFactory::GetForProfileIfExists(profile_);
    return service ? service->GetLocalChainProfiles() : std::nullopt;
  }
  void OnChainProfiles(
      const std::vector<sync_pb::CompassProfile>& profiles) override {
    if (CompassSyncService* service =
            CompassSyncServiceFactory::GetForProfileIfExists(profile_)) {
      service->OnChainProfiles(profiles);
    }
  }
  void ApplyFavicons(
      const std::vector<sync_pb::CompassFavicon>& favicons) override {
    favicon::FaviconService* service = GetFaviconService();
    if (!service) {
      return;
    }
    const std::map<std::string, std::vector<GURL>> urls =
        bridge_->GetEntryUrlsByHost();
    for (const sync_pb::CompassFavicon& favicon : favicons) {
      const std::string host = base::ToLowerASCII(favicon.host());
      auto it = urls.find(host);
      if (it == urls.end()) {
        continue;
      }
      const gfx::Image image =
          gfx::Image::CreateFrom1xPNGBytes(base::as_byte_span(favicon.png()));
      const GURL icon_url("https://" + host + kSyncedFaviconPath +
                          base::NumberToString(favicon.update_time()));
      if (image.IsEmpty() || !icon_url.is_valid()) {
        continue;
      }
      // A page that has an icon here keeps it (SetOnDemandFavicons() does
      // nothing then).
      for (const GURL& page_url : it->second) {
        service->SetOnDemandFavicons(page_url, icon_url,
                                     favicon_base::IconType::kFavicon, image,
                                     base::DoNothing());
      }
    }
  }
  void OnFaviconsWritable() override {
    favicon_sweep_timer_.Start(FROM_HERE, kFaviconSweepDelay, this,
                               &SpacesSyncHolder::OfferShownFavicons);
  }
  std::string GetProfileUuid() override {
    CompassSyncService* service =
        CompassSyncServiceFactory::GetForProfileIfExists(profile_);
    return service ? service->GetProfileUuid() : std::string();
  }
  SpaceModel* AdoptSpace(const base::Uuid& id) override {
    SpaceDirectory* directory = SpaceDirectory::GetIfExists();
    const SpaceDirectory::Entry entry =
        directory ? directory->FindSpace(id) : SpaceDirectory::Entry();
    if (!entry.space || !entry.profile || entry.profile == profile_) {
      return nullptr;
    }
    // As the user's move on the other device: the same space (ID, entries,
    // place in the switcher) leaves |entry.profile| for this one; its live
    // tabs reload here.
    ::compass::MoveSpaceToProfile(entry.profile, *entry.space, profile_);
    return service_->space_list().FindSpace(id);
  }
  std::vector<DirectorySlot> GetDirectoryOrder() override {
    std::vector<DirectorySlot> order;
    SpaceDirectory* directory = SpaceDirectory::GetIfExists();
    if (!directory || !directory->IsListed(profile_)) {
      return order;
    }
    for (const SpaceDirectory::Entry& entry : directory->GetSpaces()) {
      const std::string key = entry.space->id().AsLowercaseString();
      std::optional<syncer::UniquePosition> position;
      if (entry.profile != profile_) {
        auto it = DirectoryPositions().find(key);
        if (it != DirectoryPositions().end()) {
          position = it->second;
        }
      }
      order.emplace_back(key, std::move(position));
    }
    return order;
  }
  void OnSpacePositions(
      const std::map<std::string, syncer::UniquePosition>& positions,
      bool remote) override {
    for (const auto& space : service_->space_list().spaces()) {
      const std::string key = space->id().AsLowercaseString();
      auto it = positions.find(key);
      if (it != positions.end()) {
        DirectoryPositions().insert_or_assign(key, it->second);
      } else {
        // None (any more): a space that stays on this device keeps its
        // place in the switcher (docs/design/sync.md, "Spaces that don't
        // sync").
        DirectoryPositions().erase(key);
      }
    }
    if (remote) {
      SortDirectoryByPositions();
    }
  }

  // SpaceList::Observer:
  void OnSpaceMoved(const SpaceModel& space) override {
    PushOrderToDirectory();
  }

  // SpaceDirectory::Observer:
  void OnDirectoryChanged() override {
    // Not now: the directory can change from inside a SpaceList
    // notification (AddSpace -> SpaceDirectory::OnSpaceAdded, e.g. moving a
    // space into a new profile), and MoveSpace would notify the same
    // observer list again, which it disallows (slice 5 MacBook crash).
    // One posted pull covers a burst of changes.
    if (directory_pull_pending_) {
      return;
    }
    directory_pull_pending_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&SpacesSyncHolder::OnDirectoryChangedLater,
                                  weak_factory_.GetWeakPtr()));
  }

 private:
  void OnDirectoryChangedLater() {
    directory_pull_pending_ = false;
    PullOrderFromDirectory();
    // The switcher's order may have changed around this profile's spaces
    // (another profile's moved): their positions may need to follow.
    bridge_->NotifyLocalChange();
  }

  void OnLoaded() {
    PullOrderFromDirectory();
    if (!profile_->IsOffTheRecord()) {
      if (history::HistoryService* history =
              HistoryServiceFactory::GetForProfile(
                  profile_, ServiceAccessType::EXPLICIT_ACCESS)) {
        // A page's icon was stored: a page loaded with a new icon.
        favicons_changed_ = history->AddFaviconsChangedCallback(
            base::BindRepeating(&SpacesSyncHolder::OnFaviconsChanged,
                                weak_factory_.GetWeakPtr()));
      }
    }
    bridge_->OnModelLoaded();
  }

  // Favicons (docs/design/sync.md, "Favicons") ------------------------------

  favicon::FaviconService* GetFaviconService() {
    return profile_->IsOffTheRecord()
               ? nullptr
               : FaviconServiceFactory::GetForProfile(
                     profile_, ServiceAccessType::EXPLICIT_ACCESS);
  }

  // The URLs the profile's tabs show now: icons are taken only from pages
  // this device loaded.
  std::set<GURL> ShownUrls() const {
    std::set<GURL> urls;
    GlobalBrowserCollection::GetInstance()->ForEach(
        [&](BrowserWindowInterface* window) {
          if (window->GetProfile() != profile_) {
            return true;
          }
          TabStripModel* tabs = window->GetTabStripModel();
          for (int i = 0; tabs && i < tabs->count(); ++i) {
            urls.insert(tabs->GetWebContentsAt(i)->GetLastCommittedURL());
          }
          return true;
        });
    return urls;
  }

  void OnFaviconsChanged(const std::set<GURL>& page_urls,
                         const GURL& icon_url) {
    if (icon_url.path().starts_with(kSyncedFaviconPath)) {
      return;
    }
    const std::set<GURL> shown = ShownUrls();
    const std::set<std::string> hosts = bridge_->GetEntryHosts();
    for (const GURL& page_url : page_urls) {
      if (shown.contains(page_url)) {
        MaybeOfferFavicon(page_url, hosts);
      }
    }
  }

  void OfferShownFavicons() {
    const std::set<std::string> hosts = bridge_->GetEntryHosts();
    for (const GURL& page_url : ShownUrls()) {
      MaybeOfferFavicon(page_url, hosts);
    }
  }

  // Reads the largest icon stored for |page_url| and offers it, if an entry
  // points at its host.
  void MaybeOfferFavicon(const GURL& page_url,
                         const std::set<std::string>& hosts) {
    if (!page_url.SchemeIsHTTPOrHTTPS() || !page_url.has_host()) {
      return;
    }
    const std::string host = base::ToLowerASCII(page_url.host());
    favicon::FaviconService* service = GetFaviconService();
    if (!service || !hosts.contains(host)) {
      return;
    }
    service->GetRawFaviconForPageURL(
        page_url, {favicon_base::IconType::kFavicon},
        /*desired_size_in_pixel=*/0, /*fallback_to_host=*/false,
        base::BindOnce(&SpacesSyncHolder::OnRawFavicon,
                       weak_factory_.GetWeakPtr(), host),
        &favicon_tracker_);
  }

  void OnRawFavicon(const std::string& host,
                    const favicon_base::FaviconRawBitmapResult& result) {
    // Not one that came through sync.
    if (!result.is_valid() ||
        result.icon_url.path().starts_with(kSyncedFaviconPath)) {
      return;
    }
    std::optional<SyncedFaviconPng> png =
        MakeSyncedFaviconPng(base::span(*result.bitmap_data));
    if (png) {
      bridge_->OfferFavicon(host, png->png, png->source_size);
    }
  }

  // The profile's spaces in the switcher's order; empty if the directory
  // doesn't list them (yet).
  std::vector<SpaceModel*> DirectoryOrder() const {
    std::vector<SpaceModel*> order;
    SpaceDirectory* directory = SpaceDirectory::GetIfExists();
    if (!directory || !directory->IsListed(profile_)) {
      return order;
    }
    for (const SpaceDirectory::Entry& entry : directory->GetSpaces()) {
      if (entry.profile == profile_) {
        order.push_back(entry.space);
      }
    }
    return order;
  }

  std::vector<SpaceModel*> ListOrder() const {
    std::vector<SpaceModel*> order;
    for (const auto& space : service_->space_list().spaces()) {
      order.push_back(space.get());
    }
    return order;
  }

  // The user reordered the switcher: the list follows, and sync sends it.
  void PullOrderFromDirectory() {
    if (syncing_order_) {
      return;
    }
    const std::vector<SpaceModel*> order = DirectoryOrder();
    if (order.size() != service_->space_list().spaces().size() ||
        order == ListOrder()) {
      return;
    }
    base::AutoReset<bool> syncing(&syncing_order_, true);
    SpaceList& list = service_->space_list();
    for (size_t i = 0; i < order.size(); ++i) {
      if (list.spaces()[i].get() != order[i]) {
        list.MoveSpace(*order[i], i);
      }
    }
  }

  // Sync reordered the list: the switcher follows.
  void PushOrderToDirectory() {
    if (syncing_order_) {
      return;
    }
    SpaceDirectory* directory = SpaceDirectory::GetIfExists();
    const std::vector<SpaceModel*> list = ListOrder();
    const std::vector<SpaceModel*> order = DirectoryOrder();
    if (!directory || order.size() != list.size() || order == list) {
      return;
    }
    base::AutoReset<bool> syncing(&syncing_order_, true);
    for (size_t i = 1; i < list.size(); ++i) {
      directory->PlaceAfter(*list[i], *list[i - 1]);
    }
  }

  const raw_ptr<Profile> profile_;
  const raw_ptr<SpaceService> service_;
  std::unique_ptr<CompassSpacesSyncBridge> bridge_;
  base::WeakPtr<claude::ClaudeMemory> memory_;
  bool syncing_order_ = false;
  bool directory_pull_pending_ = false;
  base::CallbackListSubscription favicons_changed_;
  base::CancelableTaskTracker favicon_tracker_;
  base::OneShotTimer favicon_sweep_timer_;
  base::WeakPtrFactory<SpacesSyncHolder> weak_factory_{this};
};

// Runs COMPASS_SPACES only once the chain's passphrase is in place, so
// nothing is committed under a key the server knows (sync.md, "Slice 2
// notes", Passphrase).
class SpacesDataTypeController : public syncer::DataTypeController {
 public:
  SpacesDataTypeController(
      Profile* profile,
      std::unique_ptr<syncer::DataTypeControllerDelegate> full_sync,
      std::unique_ptr<syncer::DataTypeControllerDelegate> transport)
      : syncer::DataTypeController(syncer::COMPASS_SPACES,
                                   std::move(full_sync),
                                   std::move(transport)),
        profile_(profile) {}

  PreconditionState GetPreconditionState(
      const PreconditionContext& context) const override {
    CompassSyncService* service =
        CompassSyncServiceFactory::GetForProfileIfExists(profile_);
    return service && service->AreDataTypesReleased()
               ? PreconditionState::kPreconditionsMet
               : PreconditionState::kMustStopAndKeepData;
  }

 private:
  const raw_ptr<Profile> profile_;
};

// The bridge's processor, creating the bridge on first use.
base::WeakPtr<syncer::DataTypeControllerDelegate> GetControllerDelegate(
    Profile* profile) {
  SpaceService* service = SpaceService::Get(profile);
  // This is the only kind of attachment.
  auto* holder = static_cast<SpacesSyncHolder*>(service->sync_attachment());
  if (!holder) {
    auto owned = std::make_unique<SpacesSyncHolder>(profile, service);
    holder = owned.get();
    service->SetSyncAttachment(std::move(owned));
  }
  return holder->bridge().change_processor()->GetControllerDelegate();
}

syncer::DataTypeController::TypeVector CreateControllers(
    content::BrowserContext* context,
    syncer::SyncService* sync_service) {
  Profile* profile = Profile::FromBrowserContext(context);
  // The sync service, and its controllers, go before the profile.
  const syncer::ProxyDataTypeControllerDelegate::DelegateProvider provider =
      base::BindRepeating(&GetControllerDelegate, base::Unretained(profile));
  const scoped_refptr<base::SequencedTaskRunner> runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  syncer::DataTypeController::TypeVector controllers;
  // Local sync runs the types in full-sync mode; transport mode gets the
  // same bridge in case.
  controllers.push_back(std::make_unique<SpacesDataTypeController>(
      profile,
      std::make_unique<syncer::ProxyDataTypeControllerDelegate>(runner,
                                                                provider),
      std::make_unique<syncer::ProxyDataTypeControllerDelegate>(runner,
                                                                provider)));
  return controllers;
}

}  // namespace

void RegisterSpacesSyncController() {
  SetExtraControllersFactory(base::BindRepeating(&CreateControllers));
}

void NotifySpacesSyncOfLocalChange(Profile* profile) {
  SpaceService* service =
      profile ? SpaceService::GetIfExists(profile) : nullptr;
  // This is the only kind of attachment.
  auto* holder =
      service ? static_cast<SpacesSyncHolder*>(service->sync_attachment())
              : nullptr;
  if (holder) {
    holder->bridge().NotifyLocalChange();
  }
}

}  // namespace compass::sync
