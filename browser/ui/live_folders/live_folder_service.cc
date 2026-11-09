// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/live_folder_service.h"

#include <algorithm>
#include <utility>

#include "base/containers/contains.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/live_folders/live_result.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"

namespace compass {

namespace {

constexpr char kStoreFileName[] = "Compass Live Folders.json";
// The first retry after an error; it doubles with each one after.
constexpr base::TimeDelta kFirstRetry = base::Minutes(5);

// What every refresh's context loads before the plugin
// (docs/design/live-folders.md, "Runtime").
constexpr const char* kContextScripts[] = {
    "/plugins/sdk/text.js",  "/plugins/sdk/xml.js",
    "/plugins/sdk/html.js",  "/plugins/sdk/ics.js",
    "/plugins/runtime/prelude.js",
};

std::string TimeZoneId() {
  std::unique_ptr<icu::TimeZone> zone(icu::TimeZone::createDefault());
  icu::UnicodeString id;
  zone->getID(id);
  std::string utf8;
  id.toUTF8String(utf8);
  return utf8;
}

void CollectLiveFolders(SidebarNode& node,
                        base::FunctionRef<void(SidebarNode&)> callback) {
  if (node.is_live_folder()) {
    callback(node);
    return;
  }
  for (const auto& child : node.children()) {
    CollectLiveFolders(*child, callback);
  }
}

// The user's config, kept to the manifest's fields.
base::DictValue KnownFields(const LivePluginManifest& manifest,
                            const base::DictValue& config) {
  base::DictValue known;
  for (const auto& field : manifest.config) {
    if (const base::Value* value = config.Find(field.key)) {
      known.Set(field.key, value->Clone());
    }
  }
  return known;
}

}  // namespace

LiveFolderState::LiveFolderState() = default;
LiveFolderState::LiveFolderState(const LiveFolderState&) = default;
LiveFolderState& LiveFolderState::operator=(const LiveFolderState&) = default;
LiveFolderState::~LiveFolderState() = default;

LiveFolderService::LiveFolderService(Profile* profile)
    : profile_(profile),
      registry_(LivePluginRegistry::Get()),
      host_(profile),
      store_(profile->GetPath().AppendASCII(kStoreFileName)) {
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      [](base::WeakPtr<LiveFolderService> service,
         scoped_refptr<os_crypt_async::Encryptor> encryptor) {
        if (service) {
          service->store_.Load(
              std::move(encryptor),
              base::BindOnce(&LiveFolderService::OnStoreLoaded, service));
        }
      },
      weak_factory_.GetWeakPtr()));
  // Not while the profile is being made: SpaceService reads the sidebar.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<LiveFolderService> service) {
                       if (!service) {
                         return;
                       }
                       SpaceService* spaces =
                           SpaceService::Get(service->profile_);
                       service->space_service_ = spaces->GetWeakPtr();
                       spaces->RunWhenLoaded(base::BindOnce(
                           &LiveFolderService::OnSpacesLoaded, service));
                     },
                     weak_factory_.GetWeakPtr()));
}

LiveFolderService::~LiveFolderService() = default;

// static
LiveFolderService* LiveFolderService::Get(Profile* profile) {
  return LiveFolderServiceFactory::GetForProfile(profile);
}

const LiveFolderState* LiveFolderService::GetState(
    const base::Uuid& folder_id) const {
  auto it = states_.find(folder_id);
  return it == states_.end() ? nullptr : &it->second;
}

void LiveFolderService::Refresh(const base::Uuid& folder_id) {
  if (!FindFolder(folder_id)) {
    return;
  }
  LiveFolderState& state = states_[folder_id];
  if (state.refreshing || base::Contains(queue_, folder_id)) {
    return;
  }
  queue_.push_back(folder_id);
  ScheduleNext();
}

base::expected<SidebarNode*, std::string> LiveFolderService::CreateLiveFolder(
    SpaceModel& space,
    SidebarNode* parent,
    size_t index,
    std::u16string title,
    const std::string& plugin_id,
    base::DictValue config,
    const std::map<std::string, std::string>& secrets,
    bool grant_cookies) {
  const LivePlugin* plugin = registry_->Find(plugin_id);
  if (!plugin) {
    return base::unexpected("That plugin isn't installed.");
  }
  if (!space.CanAddLiveFolder(parent)) {
    return base::unexpected("Live folders go in Pinned.");
  }
  auto validated = ValidateLiveConfig(plugin->manifest, config);
  if (!validated.has_value()) {
    return base::unexpected(base::JoinString(validated.error(), ". ") + ".");
  }
  for (const auto& field : plugin->manifest.secrets) {
    auto it = secrets.find(field.key);
    const bool given = it != secrets.end() && !it->second.empty();
    if (!field.optional && !given &&
        !store_.HasSecret(plugin_id, field.key)) {
      return base::unexpected(field.label + " is required.");
    }
  }
  for (const auto& [key, value] : secrets) {
    if (!value.empty()) {
      store_.SetSecret(plugin_id, key, value);
    }
  }
  const bool title_from_plugin = title.empty();
  if (title_from_plugin) {
    title = base::UTF8ToUTF16(plugin->manifest.name);
  }
  index = std::min(index, parent->children().size());
  SidebarNode* folder = space.AddLiveFolder(
      parent, index, std::move(title),
      LiveSource(plugin_id, KnownFields(plugin->manifest, config)));
  store_.SetCookiesGranted(folder->id(),
                           grant_cookies && !plugin->manifest.cookies.empty());
  states_[folder->id()].title_from_plugin = title_from_plugin;
  Refresh(folder->id());
  return folder;
}

base::expected<void, std::string> LiveFolderService::EditLiveFolder(
    SidebarNode* folder,
    base::DictValue config,
    const std::map<std::string, std::string>& secrets,
    bool grant_cookies) {
  SpaceModel* space = nullptr;
  if (!folder || !folder->is_live_folder() ||
      !FindFolder(folder->id(), &space)) {
    return base::unexpected("That folder isn't live anymore.");
  }
  const std::string plugin_id = folder->live_source()->plugin_id;
  const LivePlugin* plugin = registry_->Find(plugin_id);
  if (!plugin) {
    return base::unexpected("That plugin isn't installed.");
  }
  auto validated = ValidateLiveConfig(plugin->manifest, config);
  if (!validated.has_value()) {
    return base::unexpected(base::JoinString(validated.error(), ". ") + ".");
  }
  for (const auto& [key, value] : secrets) {
    if (!value.empty()) {
      store_.SetSecret(plugin_id, key, value);
    }
  }
  LiveSource source = *folder->live_source();
  source.config = KnownFields(plugin->manifest, config);
  space->SetLiveSource(folder, std::move(source));
  store_.SetCookiesGranted(folder->id(),
                           grant_cookies && !plugin->manifest.cookies.empty());
  LiveFolderState& state = states_[folder->id()];
  state.failures = 0;
  state.error.reset();
  Refresh(folder->id());
  return base::ok();
}

void LiveFolderService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void LiveFolderService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void LiveFolderService::SetURLLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  loader_factory_for_testing_ = std::move(factory);
}

void LiveFolderService::SetClockForTesting(
    base::RepeatingCallback<base::Time()> now) {
  now_for_testing_ = std::move(now);
  timer_.Stop();
}

void LiveFolderService::Shutdown() {
  timer_.Stop();
  for (const auto& [folder_id, run_id] : run_ids_) {
    host_.Cancel(run_id);
  }
  run_ids_.clear();
  model_observations_.RemoveAllObservations();
  list_observation_.Reset();
  store_.Flush();
}

void LiveFolderService::OnStoreLoaded() {
  ScheduleNext();
}

void LiveFolderService::OnSpacesLoaded() {
  if (!space_service_) {
    return;
  }
  SpaceList& list = space_service_->space_list();
  list_observation_.Observe(&list);
  for (const auto& space : list.spaces()) {
    model_observations_.AddObservation(space.get());
  }
  spaces_loaded_ = true;
  ScheduleNext();
}

SidebarNode* LiveFolderService::FindFolder(const base::Uuid& id,
                                           SpaceModel** space_out) {
  if (!space_service_ || !spaces_loaded_) {
    return nullptr;
  }
  for (const auto& space : space_service_->space_list().spaces()) {
    SidebarNode* node = space->FindById(id);
    if (node && node->is_live_folder()) {
      if (space_out) {
        *space_out = space.get();
      }
      return node;
    }
  }
  return nullptr;
}

void LiveFolderService::ForEachLiveFolder(
    base::FunctionRef<void(SidebarNode&)> callback) {
  if (!space_service_ || !spaces_loaded_) {
    return;
  }
  for (const auto& space : space_service_->space_list().spaces()) {
    CollectLiveFolders(*space->pinned_root(), callback);
  }
}

void LiveFolderService::StartRefresh(const base::Uuid& folder_id) {
  SidebarNode* folder = FindFolder(folder_id);
  if (!folder) {
    states_.erase(folder_id);
    return;
  }
  const LiveSource& source = *folder->live_source();
  const LivePlugin* plugin = registry_->Find(source.plugin_id);
  if (!plugin) {
    return Fail(folder_id,
                {"plugin_missing", "The plugin " + source.plugin_id +
                                       " isn't installed on this device"});
  }
  const LivePluginManifest& manifest = plugin->manifest;
  auto config = ValidateLiveConfig(manifest, source.config);
  if (!config.has_value()) {
    return Fail(folder_id,
                {"bad_config", base::JoinString(config.error(), ". ")});
  }
  LiveSecrets stored = store_.GetSecrets(manifest.id);
  LiveSecrets secrets;
  base::DictValue secrets_dict;
  for (const auto& field : manifest.secrets) {
    auto it = stored.find(field.key);
    if (it != stored.end()) {
      secrets[field.key] = it->second;
      secrets_dict.Set(field.key, it->second);
    } else if (!field.optional) {
      return Fail(folder_id, {"missing_secret", field.label + " is not set"});
    }
  }
  const base::Time now = Now();
  LiveFolderHost::RunSpec spec;
  for (const char* script : kContextScripts) {
    spec.scripts.push_back(script);
  }
  spec.scripts.push_back(plugin->EntryPath());
  spec.env = base::DictValue()
                 .Set("now", now.InMillisecondsFSinceUnixEpoch())
                 .Set("locale", g_browser_process->GetApplicationLocale())
                 .Set("timeZone", TimeZoneId())
                 .Set("secrets", std::move(secrets_dict));
  spec.max_items = static_cast<int>(kMaxLiveItems);
  spec.fetcher = std::make_unique<LiveFetcher>(
      GetURLLoaderFactory(), ResolveLiveHosts(manifest, *config, secrets),
      store_.CookiesGranted(folder_id), LiveFetcher::Limits());
  spec.config = std::move(*config);

  LiveFolderState& state = states_[folder_id];
  state.refreshing = true;
  state.last_attempt = now;
  run_ids_[folder_id] = host_.Run(
      std::move(spec),
      base::BindOnce(&LiveFolderService::OnRefreshDone,
                     weak_factory_.GetWeakPtr(), folder_id, now,
                     manifest.refresh_default, manifest.refresh_min));
  NotifyStateChanged(folder_id);
}

void LiveFolderService::OnRefreshDone(
    base::Uuid folder_id,
    base::Time started,
    base::TimeDelta plugin_default,
    base::TimeDelta plugin_min,
    base::expected<std::string, LiveError> result,
    std::vector<std::string> log) {
  run_ids_.erase(folder_id);
  SpaceModel* space = nullptr;
  SidebarNode* folder = FindFolder(folder_id, &space);
  if (!folder) {
    states_.erase(folder_id);
    ScheduleNext();
    return;
  }
  LiveFolderState& state = states_[folder_id];
  state.refreshing = false;
  state.log = std::move(log);
  if (!result.has_value()) {
    return Fail(folder_id, result.error());
  }
  auto parsed = ParseLiveResult(*result);
  if (!parsed.has_value()) {
    return Fail(folder_id, parsed.error());
  }
  LiveTabRegistry* tabs =
      space_service_ ? &space_service_->registry() : nullptr;
  space->ReplaceLiveItems(folder, parsed->items, Now(),
                          [tabs](const SidebarNode& node) {
                            return tabs && tabs->GetTab(&node);
                          });
  if (state.title_from_plugin && !parsed->title.empty()) {
    space->SetTitle(folder, parsed->title);
  }
  state.title_from_plugin = false;
  state.error.reset();
  state.failures = 0;
  const base::TimeDelta interval = std::clamp(
      parsed->next_refresh.value_or(plugin_default), plugin_min, kMaxInterval);
  state.next_refresh = started + interval;
  NotifyStateChanged(folder_id);
  ScheduleNext();
}

void LiveFolderService::Fail(const base::Uuid& folder_id, LiveError error) {
  LiveFolderState& state = states_[folder_id];
  state.refreshing = false;
  state.error = std::move(error);
  ++state.failures;
  base::TimeDelta delay = kFirstRetry;
  for (int i = 1; i < state.failures && delay < kMaxInterval; ++i) {
    delay *= 2;
  }
  state.next_refresh = Now() + std::min(delay, kMaxInterval);
  NotifyStateChanged(folder_id);
  ScheduleNext();
}

void LiveFolderService::ScheduleNext() {
  if (!spaces_loaded_ || !store_.loaded()) {
    return;
  }
  const base::Time now = Now();
  // Folders that are due join the queue.
  base::Time next;
  ForEachLiveFolder([&](SidebarNode& folder) {
    auto [it, inserted] = states_.try_emplace(folder.id());
    LiveFolderState& state = it->second;
    if (inserted) {
      state.collapsed = folder.collapsed();
    }
    if (state.refreshing || base::Contains(queue_, folder.id())) {
      return;
    }
    if (state.next_refresh.is_null()) {
      // Never refreshed here (this run): when the saved entries are due.
      const LivePlugin* plugin =
          registry_->Find(folder.live_source()->plugin_id);
      const base::TimeDelta interval =
          plugin ? plugin->manifest.refresh_default
                 : LivePluginManifest::kDefaultRefresh;
      state.next_refresh = folder.live_refreshed().is_null()
                               ? now
                               : folder.live_refreshed() + interval;
    }
    if (state.next_refresh <= now) {
      queue_.push_back(folder.id());
    } else if (next.is_null() || state.next_refresh < next) {
      next = state.next_refresh;
    }
  });
  // Start what fits.
  while (!queue_.empty() && run_ids_.size() < kMaxConcurrentRefreshes) {
    const base::Uuid folder_id = queue_.front();
    queue_.erase(queue_.begin());
    StartRefresh(folder_id);
  }
  if (now_for_testing_) {
    return;
  }
  if (next.is_null()) {
    timer_.Stop();
  } else {
    timer_.Start(FROM_HERE, std::max(next - now, base::Seconds(1)),
                 base::BindOnce(&LiveFolderService::ScheduleNext,
                                base::Unretained(this)));
  }
}

void LiveFolderService::RefreshIfStale(const SidebarNode& folder) {
  const LiveFolderState* state = GetState(folder.id());
  const base::Time now = Now();
  if (state && (state->refreshing ||
                (!state->last_attempt.is_null() &&
                 now - state->last_attempt < kStaleAfter))) {
    return;
  }
  if (!folder.live_refreshed().is_null() &&
      now - folder.live_refreshed() < kStaleAfter) {
    return;
  }
  Refresh(folder.id());
}

void LiveFolderService::OnSpaceAdded(SpaceModel& space) {
  model_observations_.AddObservation(&space);
  ScheduleNext();
}

void LiveFolderService::OnSpaceWillBeRemoved(SpaceModel& space) {
  CollectLiveFolders(*space.pinned_root(), [this](SidebarNode& folder) {
    if (auto it = run_ids_.find(folder.id()); it != run_ids_.end()) {
      host_.Cancel(it->second);
      run_ids_.erase(it);
    }
    std::erase(queue_, folder.id());
    states_.erase(folder.id());
  });
  model_observations_.RemoveObservation(&space);
}

void LiveFolderService::OnActiveSpaceChanged() {
  if (!space_service_ || !spaces_loaded_) {
    return;
  }
  CollectLiveFolders(
      *space_service_->space_list().active_space().pinned_root(),
      [this](SidebarNode& folder) { RefreshIfStale(folder); });
}

void LiveFolderService::OnNodeAdded(const SidebarNode& node) {
  bool live = false;
  CollectLiveFolders(const_cast<SidebarNode&>(node),
                     [&live](SidebarNode&) { live = true; });
  if (live) {
    ScheduleNext();
  }
}

void LiveFolderService::OnNodeChanged(const SidebarNode& node) {
  if (node.type() != SidebarNode::Type::kFolder) {
    return;
  }
  auto it = states_.find(node.id());
  if (!node.is_live_folder()) {
    // Converted to a plain folder.
    if (it != states_.end()) {
      if (auto run = run_ids_.find(node.id()); run != run_ids_.end()) {
        host_.Cancel(run->second);
        run_ids_.erase(run);
      }
      std::erase(queue_, node.id());
      states_.erase(it);
      store_.SetCookiesGranted(node.id(), false);
      NotifyStateChanged(node.id());
      ScheduleNext();
    }
    return;
  }
  // Expanded: show what's new.
  if (it == states_.end()) {
    states_[node.id()].collapsed = node.collapsed();
    return;
  }
  const bool was_collapsed = it->second.collapsed;
  it->second.collapsed = node.collapsed();
  if (was_collapsed && !node.collapsed()) {
    RefreshIfStale(node);
  }
}

void LiveFolderService::OnNodeWillBeRemoved(const SidebarNode& node) {
  CollectLiveFolders(const_cast<SidebarNode&>(node),
                     [this](SidebarNode& folder) {
                       if (auto it = run_ids_.find(folder.id());
                           it != run_ids_.end()) {
                         host_.Cancel(it->second);
                         run_ids_.erase(it);
                       }
                       std::erase(queue_, folder.id());
                       states_.erase(folder.id());
                       store_.SetCookiesGranted(folder.id(), false);
                     });
}

base::Time LiveFolderService::Now() const {
  return now_for_testing_ ? now_for_testing_.Run() : base::Time::Now();
}

scoped_refptr<network::SharedURLLoaderFactory>
LiveFolderService::GetURLLoaderFactory() {
  if (loader_factory_for_testing_) {
    return loader_factory_for_testing_;
  }
  return profile_->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess();
}

void LiveFolderService::NotifyStateChanged(const base::Uuid& folder_id) {
  for (Observer& observer : observers_) {
    observer.OnLiveFolderStateChanged(folder_id);
  }
}

// static
LiveFolderServiceFactory* LiveFolderServiceFactory::GetInstance() {
  static base::NoDestructor<LiveFolderServiceFactory> instance;
  return instance.get();
}

// static
LiveFolderService* LiveFolderServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<LiveFolderService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

LiveFolderServiceFactory::LiveFolderServiceFactory()
    : ProfileKeyedServiceFactory(
          "CompassLiveFolderService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

LiveFolderServiceFactory::~LiveFolderServiceFactory() = default;

std::unique_ptr<KeyedService>
LiveFolderServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<LiveFolderService>(
      Profile::FromBrowserContext(context));
}

bool LiveFolderServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

bool LiveFolderServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace compass
