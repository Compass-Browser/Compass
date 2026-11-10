// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/live_folder_service.h"

#include <algorithm>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/live_folders/live_result.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"

#if !BUILDFLAG(IS_ANDROID)
#include "chrome/browser/platform_util.h"
#endif

namespace compass {

namespace {

constexpr char kStoreFileName[] = "Compass Live Folders.json";
// The first retry after an error; it doubles with each one after.
constexpr base::TimeDelta kFirstRetry = base::Minutes(5);

// Runs once the installed plugins have been read (the first service reads
// them, for the whole browser).
std::vector<base::OnceClosure>& WhenPluginsLoaded() {
  static base::NoDestructor<std::vector<base::OnceClosure>> callbacks;
  return *callbacks;
}
bool g_reading_plugins = false;

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

base::expected<LiveFolderHost::RunSpec, LiveError> MakeLiveRunSpec(
    const LivePlugin& plugin,
    const base::DictValue& config,
    const LiveSecrets& secrets,
    bool cookies_granted,
    base::DictValue env,
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    LiveFetcher::Limits limits,
    base::TimeDelta timeout) {
  const LivePluginManifest& manifest = plugin.manifest;
  auto validated = ValidateLiveConfig(manifest, config);
  if (!validated.has_value()) {
    return base::unexpected(
        LiveError{"bad_config", base::JoinString(validated.error(), ". ")});
  }
  // Only the plugin's own, declared secrets.
  LiveSecrets own;
  base::DictValue secrets_dict;
  for (const auto& field : manifest.secrets) {
    auto it = secrets.find(field.key);
    if (it != secrets.end() && !it->second.empty()) {
      own[field.key] = it->second;
      secrets_dict.Set(field.key, it->second);
    } else if (!field.optional) {
      return base::unexpected(
          LiveError{"missing_secret", field.label + " is not set"});
    }
  }
  LiveFolderHost::RunSpec spec;
  for (const char* script : kContextScripts) {
    spec.scripts.push_back(script);
  }
  spec.scripts.push_back(plugin.EntryPath());
  spec.env = std::move(env);
  spec.env.Set("secrets", std::move(secrets_dict));
  spec.timeout = timeout;
  spec.max_items = static_cast<int>(kMaxLiveItems);
  spec.fetcher = std::make_unique<LiveFetcher>(
      std::move(loader_factory), ResolveLiveHosts(manifest, *validated, own),
      cookies_granted, limits);
  spec.config = std::move(*validated);
  return spec;
}

LiveFolderState::LiveFolderState() = default;
LiveFolderState::LiveFolderState(const LiveFolderState&) = default;
LiveFolderState& LiveFolderState::operator=(const LiveFolderState&) = default;
LiveFolderState::~LiveFolderState() = default;

LiveFolderService::LiveFolderService(Profile* profile,
                                     std::unique_ptr<LiveFolderSpaces> spaces)
    : profile_(profile),
      registry_(LivePluginRegistry::Get()),
      host_(profile),
      store_(profile->GetPath().AppendASCII(kStoreFileName)),
      spaces_(std::move(spaces)) {
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
  LivePluginRegistry& registry = LivePluginRegistry::Get();
  if (!registry.installed_loaded()) {
    WhenPluginsLoaded().push_back(base::BindOnce(
        &LiveFolderService::OnPluginsLoaded, weak_factory_.GetWeakPtr()));
    if (!g_reading_plugins) {
      g_reading_plugins = true;
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&ReadLivePluginPackages, plugins_dir()),
          base::BindOnce([](std::vector<LivePluginPackage> packages) {
            LivePluginRegistry& registry = LivePluginRegistry::Get();
            for (const LivePluginPackage& package : packages) {
              registry.AddInstalled(package);
            }
            registry.set_installed_loaded();
            std::vector<base::OnceClosure> callbacks =
                std::move(WhenPluginsLoaded());
            for (base::OnceClosure& callback : callbacks) {
              std::move(callback).Run();
            }
          }));
    }
  }
  spaces_->RunWhenLoaded(base::BindOnce(&LiveFolderService::OnSpacesLoaded,
                                        weak_factory_.GetWeakPtr()));
}

LiveFolderService::~LiveFolderService() = default;

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
  if (state.refreshing || std::ranges::contains(queue_, folder_id)) {
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
      LiveSource(plugin_id, KnownFields(plugin->manifest, config),
                 plugin->source, plugin->sha256));
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

void LiveFolderService::OnPluginsLoaded() {
  ScheduleNext();
}

base::FilePath LiveFolderService::plugins_dir() const {
  return LivePluginsDir(profile_->GetPath().DirName());
}

void LiveFolderService::PreviewPlugin(const GURL& url,
                                      LivePluginPackageCallback callback) {
  FetchLivePluginPackage(GetURLLoaderFactory(), url, std::move(callback));
}

void LiveFolderService::InstallPlugin(
    const GURL& url,
    const std::string& expected_sha256,
    base::OnceCallback<void(const std::string&)> callback) {
  FetchLivePluginPackage(
      GetURLLoaderFactory(), url,
      base::BindOnce(
          [](base::WeakPtr<LiveFolderService> service, std::string expected,
             base::OnceCallback<void(const std::string&)> callback,
             base::expected<LivePluginPackage, std::string> package) {
            if (!package.has_value()) {
              std::move(callback).Run(package.error());
              return;
            }
            if (package->sha256 != expected) {
              std::move(callback).Run(
                  "The plugin at that address has changed since. Review it "
                  "again before installing.");
              return;
            }
            if (!service) {
              std::move(callback).Run("Compass is closing.");
              return;
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
                base::BindOnce(&WriteLivePluginPackage, service->plugins_dir(),
                               *package),
                base::BindOnce(
                    [](base::WeakPtr<LiveFolderService> service,
                       LivePluginPackage package,
                       base::OnceCallback<void(const std::string&)> callback,
                       bool written) {
                      if (!written) {
                        std::move(callback).Run(
                            "The plugin couldn't be saved on this device.");
                        return;
                      }
                      LivePluginRegistry::Get().AddInstalled(package);
                      std::move(callback).Run(std::string());
                      if (service) {
                        // Folders that were waiting for it.
                        service->ScheduleNext();
                      }
                    },
                    service, std::move(*package), std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), expected_sha256, std::move(callback)));
}

void LiveFolderService::RemovePlugin(const std::string& id,
                                     base::OnceClosure done) {
  const LivePlugin* plugin = registry_->Find(id);
  if (!plugin || plugin->builtin) {
    std::move(done).Run();
    return;
  }
  registry_->RemoveInstalled(id);
  store_.RemoveSecrets(id);
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(base::IgnoreResult(&DeleteLivePluginPackage),
                     plugins_dir(), id),
      std::move(done));
}

void LiveFolderService::RescanPlugins(base::OnceClosure done) {
  if (!registry_->installed_loaded()) {
    // The first read is still on its way; it reads local plugins too.
    WhenPluginsLoaded().push_back(std::move(done));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ReadLivePluginPackages, plugins_dir()),
      base::BindOnce(
          [](base::WeakPtr<LiveFolderService> service, base::OnceClosure done,
             std::vector<LivePluginPackage> packages) {
            LivePluginRegistry::Get().ReplaceInstalled(packages);
            std::move(done).Run();
            if (service) {
              // Folders that were waiting for a plugin that's now here.
              service->ScheduleNext();
            }
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void LiveFolderService::OpenPluginsFolder() {
#if BUILDFLAG(IS_ANDROID)
  NOTIMPLEMENTED();
#else
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(
          [](base::FilePath dir) { return base::CreateDirectory(dir); },
          plugins_dir()),
      base::BindOnce(
          [](base::WeakPtr<LiveFolderService> service, bool made) {
            if (service && made) {
              platform_util::OpenItem(service->profile_,
                                      service->plugins_dir(),
                                      platform_util::OPEN_FOLDER,
                                      base::DoNothing());
            }
          },
          weak_factory_.GetWeakPtr()));
#endif  // BUILDFLAG(IS_ANDROID)
}

void LiveFolderService::OnSpacesLoaded() {
  SpaceList* list = spaces_->list();
  if (!list) {
    return;
  }
  list_observation_.Observe(list);
  for (const auto& space : list->spaces()) {
    model_observations_.AddObservation(space.get());
  }
  spaces_loaded_ = true;
  ScheduleNext();
}

SidebarNode* LiveFolderService::FindFolder(const base::Uuid& id,
                                           SpaceModel** space_out) {
  if (!spaces_loaded_ || !spaces_->list()) {
    return nullptr;
  }
  for (const auto& space : spaces_->list()->spaces()) {
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
  if (!spaces_loaded_ || !spaces_->list()) {
    return;
  }
  for (const auto& space : spaces_->list()->spaces()) {
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
  const base::Time now = Now();
  auto spec = MakeLiveRunSpec(
      *plugin, source.config, store_.GetSecrets(manifest.id),
      store_.CookiesGranted(folder_id),
      base::DictValue()
          .Set("now", now.InMillisecondsFSinceUnixEpoch())
          .Set("locale", g_browser_process->GetApplicationLocale())
          .Set("timeZone", TimeZoneId()),
      GetURLLoaderFactory());
  if (!spec.has_value()) {
    return Fail(folder_id, spec.error());
  }

  LiveFolderState& state = states_[folder_id];
  state.refreshing = true;
  state.last_attempt = now;
  run_ids_[folder_id] = host_.Run(
      std::move(*spec),
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
  space->ReplaceLiveItems(folder, parsed->items, Now(),
                          [this](const SidebarNode& node) {
                            return spaces_->HasTab(node);
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
  if (!spaces_loaded_ || !store_.loaded() ||
      !registry_->installed_loaded()) {
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
    if (state.refreshing || std::ranges::contains(queue_, folder.id())) {
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
  if (!spaces_loaded_ || !spaces_->list()) {
    return;
  }
  CollectLiveFolders(
      *spaces_->list()->active_space().pinned_root(),
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

}  // namespace compass
