// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/room_service.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "base/barrier_closure.h"
#include "base/base64.h"
#include "base/base64url.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "chrome/browser/sync/device_info_sync_service_factory.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/easels/easel_sync.h"
#include "compass/browser/rooms/os_user_name.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/components/sync/pref_names.h"
#include "compass/components/sync/sync_hooks.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/sync/base/command_line_switches.h"
#include "components/sync_device_info/device_info.h"
#include "components/sync_device_info/device_info_sync_service.h"
#include "components/sync_device_info/local_device_info_provider.h"
#include "content/public/browser/storage_partition.h"
#include "ui/base/l10n/l10n_util.h"

#if BUILDFLAG(IS_ANDROID)
#include "compass/browser/android/spaces_android.h"
#else
#include "compass/browser/window_sync/space_service.h"
#endif

namespace compass::rooms {

namespace {

using sync::rooms::RoomClient;
using sync::rooms::RoomError;
using sync::rooms::RoomId;
using sync::rooms::RoomInfo;
using sync::rooms::RoomKey;
using sync::rooms::RoomResult;

// Whether what observers show of a room (people, roles, invites) differs.
bool ShownDiffers(const RoomInfo& a, const RoomInfo& b) {
  auto member = [](const sync::rooms::RoomMember& m) {
    return std::tie(m.client_id, m.role, m.profile, m.name, m.email,
                    m.pending);
  };
  auto invite = [](const sync::rooms::RoomInvite& i) {
    return std::tie(i.id, i.role, i.expires, i.uses_left, i.revoked);
  };
  if (a.role != b.role || a.pending != b.pending || a.epoch != b.epoch ||
      a.members.size() != b.members.size() ||
      a.invites.size() != b.invites.size()) {
    return true;
  }
  for (size_t i = 0; i < a.members.size(); ++i) {
    if (member(a.members[i]) != member(b.members[i])) {
      return true;
    }
  }
  for (size_t i = 0; i < a.invites.size(); ++i) {
    if (invite(a.invites[i]) != invite(b.invites[i])) {
      return true;
    }
  }
  return false;
}

constexpr base::FilePath::CharType kFileName[] =
    FILE_PATH_LITERAL("Compass Rooms.json");
constexpr base::TimeDelta kSaveDelay = base::Seconds(2);
// Members, roles and keys are read again this often (room events aren't
// followed yet; the changes feed is).
constexpr base::TimeDelta kRefreshInterval = base::Minutes(1);
// The record ID a member's profile is sealed under.
// The record ID a member's profile is sealed under: "profile:" + its chain ID
// (lowercase), as iOS's.
std::string ProfileRecordId(std::string_view client_id) {
  return "profile:" + base::ToLowerASCII(client_id);
}

std::string RoomIdText(const RoomId& id) {
  std::string text;
  base::Base64UrlEncode(id, base::Base64UrlEncodePolicy::OMIT_PADDING, &text);
  return text;
}

std::optional<RoomId> ParseRoomId(const std::string& text) {
  std::optional<std::vector<uint8_t>> bytes = base::Base64UrlDecode(
      text, base::Base64UrlDecodePolicy::DISALLOW_PADDING);
  if (!bytes || bytes->size() != sync::rooms::kRoomIdSize) {
    return std::nullopt;
  }
  RoomId id;
  std::ranges::copy(*bytes, id.begin());
  return id;
}

std::string SecretText(const sync::rooms::InviteSecret& secret) {
  std::string text;
  base::Base64UrlEncode(secret, base::Base64UrlEncodePolicy::OMIT_PADDING,
                        &text);
  return text;
}

std::string ErrorText(const RoomError& error) {
  if (error.is_network()) {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_NETWORK);
  }
  if (error.is_login_required()) {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_LOGIN_REQUIRED);
  }
  if (error.is_chain_not_allowed()) {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_CHAIN_NOT_ALLOWED);
  }
  if (error.is_gone()) {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_GONE);
  }
  if (error.is_forbidden()) {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_FORBIDDEN);
  }
  if (error.is_quota_exceeded()) {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_QUOTA);
  }
  if (error.code == "invite_expired") {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_INVITE_EXPIRED);
  }
  if (error.code == "invite_used_up") {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_INVITE_USED_UP);
  }
  if (error.code == "invite_revoked" || error.code == "invite_invalid") {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_INVITE_REVOKED);
  }
  if (error.code == "room_full") {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_ROOM_FULL);
  }
  if (error.code == "owner_must_transfer") {
    return l10n_util::GetStringUTF8(
        IDS_COMPASS_ROOMS_ERROR_OWNER_MUST_TRANSFER);
  }
  if (error.code == "removed") {
    return l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_REMOVED);
  }
  return error.code.empty()
             ? l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_UNKNOWN)
             : l10n_util::GetStringFUTF8(IDS_COMPASS_ROOMS_ERROR_WITH_CODE,
                                         base::UTF8ToUTF16(error.code));
}

std::map<uint32_t, RoomKey> KeysOf(const sync_pb::CompassRoomMount& mount) {
  std::map<uint32_t, RoomKey> keys;
  for (const sync_pb::CompassRoomKey& key : mount.keys()) {
    if (key.key().size() == sync::rooms::kRoomKeySize) {
      RoomKey k;
      std::ranges::copy(key.key(), k.begin());
      keys[key.epoch()] = k;
    }
  }
  return keys;
}

void SetKeys(sync_pb::CompassRoomMount& mount,
             const std::map<uint32_t, RoomKey>& keys) {
  mount.clear_keys();
  for (const auto& [epoch, key] : keys) {
    sync_pb::CompassRoomKey& entry = *mount.add_keys();
    entry.set_epoch(epoch);
    entry.set_key(std::string(key.begin(), key.end()));
  }
}

std::string NameOfItem(SpaceList* list,
                       const easels::EaselStore* easels,
                       const std::string& kind,
                       const std::string& root_uuid) {
  const base::Uuid id = base::Uuid::ParseLowercase(root_uuid);
  if (kind == "space" && list) {
    if (SpaceModel* space = list->FindSpace(id)) {
      return base::UTF16ToUTF8(space->name());
    }
  } else if (kind == "folder" && list) {
    for (const auto& space : list->spaces()) {
      if (SidebarNode* node = space->FindById(id)) {
        return base::UTF16ToUTF8(node->title());
      }
    }
  } else if (kind == "easel" && easels) {
    if (const sync_pb::CompassEasel* easel = easels->FindEasel(root_uuid)) {
      return easel->title();
    }
  }
  return std::string();
}

class RoomServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static RoomServiceFactory* GetInstance() {
    static base::NoDestructor<RoomServiceFactory> instance;
    return instance.get();
  }

  RoomService* Get(Profile* profile, bool create) {
    return static_cast<RoomService*>(
        GetServiceForBrowserContext(profile, create));
  }

 private:
  friend base::NoDestructor<RoomServiceFactory>;

  RoomServiceFactory()
      : ProfileKeyedServiceFactory(
            "CompassRoomService",
            ProfileSelections::Builder()
                .WithRegular(ProfileSelection::kOriginalOnly)
                .WithGuest(ProfileSelection::kNone)
                .WithSystem(ProfileSelection::kNone)
                .WithAshInternals(ProfileSelection::kNone)
                .Build()) {
    DependsOn(easels::EaselStoreFactory::GetInstance());
    DependsOn(DeviceInfoSyncServiceFactory::GetInstance());
  }
  ~RoomServiceFactory() override = default;

  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override {
    return std::make_unique<RoomService>(Profile::FromBrowserContext(context));
  }
};

}  // namespace

// The profile's Easels for rooms, and their changes for the service.
class RoomService::EaselSourceImpl : public EaselSource,
                                     public easels::EaselStore::Observer {
 public:
  EaselSourceImpl(RoomService* service, easels::EaselStore* store)
      : service_(service), store_(store ? store->GetWeakPtr() : nullptr) {
    if (store_) {
      store_->AddObserver(this);
    }
  }
  ~EaselSourceImpl() override {
    if (store_) {
      store_->RemoveObserver(this);
    }
  }

  easels::EaselStore* store() const { return store_.get(); }

  std::optional<std::vector<sync_pb::CompassSpacesSpecifics>> Entities()
      const override {
    return easels::LocalEaselEntities(store_.get());
  }
  std::optional<std::string> AssetData(
      const std::string& sha256) const override {
    return easels::EaselAssetData(store_.get(), sha256);
  }
  void Apply(const std::vector<sync_pb::CompassSpacesSpecifics>& changed,
             const std::set<std::string>& deleted) override {
    easels::ApplyRemoteEasels(store_.get(), changed, deleted);
  }

  // easels::EaselStore::Observer:
  void OnEaselsChanged() override { service_->OnEaselsChanged(); }

 private:
  const raw_ptr<RoomService> service_;
  base::WeakPtr<easels::EaselStore> store_;
};

// A room's RoomSync::Delegate.
class RoomService::RoomHolder : public RoomSync::Delegate {
 public:
  RoomHolder(RoomService* service, std::string room_id)
      : service_(service), room_id_(std::move(room_id)) {}

  void OnKeys(const std::map<uint32_t, RoomKey>& keys) override {
    service_->OnRoomKeys(room_id_, keys);
  }
  void OnRoom(const RoomInfo& room) override {
    service_->OnRoomInfo(room_id_, room);
  }
  void OnStateChanged() override { service_->ScheduleSave(); }
  void OnRemoved(bool room_deleted) override {
    // Not from inside the room's sync, which this destroys.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&RoomService::OnRoomRemoved,
                                  service_->weak_factory_.GetWeakPtr(),
                                  room_id_, room_deleted));
  }
  void OnWriteRefused(const RoomError& error) override {
    LOG(WARNING) << "[compass-rooms] a change to " << room_id_
                 << " was refused: " << error.code;
  }

 private:
  const raw_ptr<RoomService> service_;
  const std::string room_id_;
};

struct RoomService::Room {
  sync_pb::CompassRoomMount mount;
  RoomId id{};
  RoomState state;
  // Members the owner sent every key to (members who joined by an older
  // link need the newer ones).
  std::set<std::string> keyed_members;
  std::optional<RoomInfo> info;
  std::unique_ptr<RoomHolder> holder;
  std::unique_ptr<RoomSync> sync;
  bool discovering = false;
  // Join() callbacks waiting for the root.
  std::vector<base::OnceCallback<void(const std::string&, const std::string&)>> joins;
};

RoomView::RoomView() = default;
RoomView::RoomView(const RoomView&) = default;
RoomView& RoomView::operator=(const RoomView&) = default;
RoomView::~RoomView() = default;

// static
RoomService* RoomService::Get(Profile* profile) {
  return RoomServiceFactory::GetInstance()->Get(profile, /*create=*/true);
}

// static
RoomService* RoomService::GetIfExists(Profile* profile) {
  return RoomServiceFactory::GetInstance()->Get(profile, /*create=*/false);
}

// static
void RoomService::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  // compass::RegisterCompassProfilePrefs() registers kDisplayNamePref (the
  // factory is built after profiles' prefs are): this is for tests.
  registry->RegisterStringPref(kDisplayNamePref, std::string());
}

// static
void RoomService::EnsureFactoryBuilt() {
  RoomServiceFactory::GetInstance();
}

RoomService::RoomService(Profile* profile)
    : profile_(profile),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  writer_ = std::make_unique<base::ImportantFileWriter>(
      profile->GetPath().Append(kFileName), file_task_runner_, kSaveDelay,
      "CompassRooms");
  easels_ = std::make_unique<EaselSourceImpl>(
      this, easels::EaselStore::Get(profile));
  PrefService* prefs = profile->GetPrefs();
  pref_change_registrar_.Init(prefs);
  pref_change_registrar_.Add(
      sync::prefs::kSeed,
      base::BindRepeating(&RoomService::OnSeedPrefChanged,
                          base::Unretained(this)));

  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](const base::FilePath& path) -> std::optional<std::string> {
            std::string contents;
            if (!base::ReadFileToString(path, &contents)) {
              return std::nullopt;
            }
            return contents;
          },
          writer_->path()),
      base::BindOnce(&RoomService::OnFileRead, weak_factory_.GetWeakPtr()));

  // The account: the chain's (home profile) or the profile's own.
  account_uuid_ = prefs->GetBoolean(sync::prefs::kProfileAccount)
                      ? prefs->GetString(sync::prefs::kProfileUuid)
                      : std::string();
  const GURL upstream(base::CommandLine::ForCurrentProcess()
                          ->GetSwitchValueASCII(syncer::kSyncServiceURL));
  own_server_ = sync::GetServerUrl(prefs, upstream);
  if (!prefs->GetString(sync::prefs::kSeed).empty() &&
      !prefs->GetBoolean(sync::prefs::kResetPending) &&
      g_browser_process->os_crypt_async()) {
    g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
        &RoomService::OnEncryptor, weak_factory_.GetWeakPtr()));
  }

#if BUILDFLAG(IS_ANDROID)
  if (SpacesAndroid* spaces = SpacesAndroid::Get(profile)) {
    list_ = &spaces->list();
    spaces->RunWhenLoaded(base::BindOnce(&RoomService::OnSpacesLoaded,
                                         weak_factory_.GetWeakPtr()));
  }
#else
  if (SpaceService* spaces = SpaceService::Get(profile)) {
    list_ = &spaces->space_list();
    spaces->RunWhenLoaded(base::BindOnce(&RoomService::OnSpacesLoaded,
                                         weak_factory_.GetWeakPtr()));
  }
#endif
}

RoomService::~RoomService() = default;

void RoomService::Shutdown() {
  if (writer_ && writer_->HasPendingWrite()) {
    writer_->DoScheduledWrite();
  }
  rooms_.clear();
  clients_.clear();
  if (list_) {
    list_->RemoveObserver(this);
    for (const auto& space : list_->spaces()) {
      space->RemoveObserver(this);
    }
  }
  easels_.reset();
  refresh_timer_.Stop();
  stopped_ = true;
}

void RoomService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void RoomService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

bool RoomService::IsAvailable() const {
  return !stopped_ && identity_ && file_loaded_ && spaces_loaded_ &&
         own_server_.is_valid();
}

void RoomService::OnEncryptor(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  std::optional<std::vector<uint8_t>> encrypted = base::Base64Decode(
      profile_->GetPrefs()->GetString(sync::prefs::kSeed));
  std::optional<std::string> plain = encryptor && encrypted
                                         ? encryptor->DecryptData(*encrypted)
                                         : std::nullopt;
  if (!plain || plain->size() != sync::kSeedSize) {
    LOG(ERROR) << "[compass-rooms] can't read the chain's seed";
    return;
  }
  sync::Seed seed;
  std::ranges::copy(*plain, seed.begin());
  seed_ = seed;
  auth_key_ = sync::DeriveAuthKey(seed, account_uuid_);
  identity_ = sync::rooms::DeriveIdentityKey(seed, account_uuid_);
  MaybeStart();
}

void RoomService::OnSpacesLoaded() {
  spaces_loaded_ = true;
  list_->AddObserver(this);
  for (const auto& space : list_->spaces()) {
    space->AddObserver(this);
  }
  MaybeStart();
}

void RoomService::OnFileRead(std::optional<std::string> contents) {
  file_loaded_ = true;
  std::optional<base::Value> value =
      contents ? base::JSONReader::Read(*contents, base::JSON_PARSE_RFC)
               : std::nullopt;
  if (value && value->is_dict()) {
    const base::DictValue& dict = value->GetDict();
    if (const base::DictValue* rooms = dict.FindDict("rooms")) {
      for (const auto [room_id, entry] : *rooms) {
        if (!entry.is_dict()) {
          continue;
        }
        const std::string* mount_text = entry.GetDict().FindString("mount");
        std::string bytes;
        sync_pb::CompassRoomMount mount;
        if (!mount_text || !base::Base64Decode(*mount_text, &bytes) ||
            !mount.ParseFromString(bytes) || mount.room_id() != room_id) {
          continue;
        }
        std::optional<RoomId> id = ParseRoomId(room_id);
        if (!id) {
          continue;
        }
        auto room = std::make_unique<Room>();
        room->mount = std::move(mount);
        room->id = *id;
        if (const base::DictValue* state = entry.GetDict().FindDict("state")) {
          room->state = RoomState::FromValue(*state);
        }
        if (const base::ListValue* keyed = entry.GetDict().FindList("keyed")) {
          for (const base::Value& member : *keyed) {
            if (member.is_string()) {
              room->keyed_members.insert(member.GetString());
            }
          }
        }
        rooms_[room_id] = std::move(room);
      }
    }
    if (const base::ListValue* notices = dict.FindList("notices")) {
      for (const base::Value& notice : *notices) {
        if (!notice.is_dict()) {
          continue;
        }
        const base::DictValue& n = notice.GetDict();
        notices_.push_back(
            {n.FindString("room_id") ? *n.FindString("room_id") : "",
             n.FindString("name") ? *n.FindString("name") : "",
             n.FindString("kind") ? *n.FindString("kind") : "",
             n.FindBool("deleted").value_or(false)});
      }
    }
    if (const base::ListValue* servers = dict.FindList("identity_servers")) {
      for (const base::Value& server : *servers) {
        if (server.is_string()) {
          identity_servers_.insert(server.GetString());
        }
      }
    }
  }
  MaybeStart();
  // The mounts are in: the personal chain may sync them.
  NotifyRoots();
}

void RoomService::MaybeStart() {
  if (!IsAvailable()) {
    return;
  }
  VLOG(1) << "[compass-rooms] " << rooms_.size() << " rooms for "
          << profile_->GetPath().BaseName().AsUTF8Unsafe();
  for (auto& [room_id, room] : rooms_) {
    StartRoom(*room);
  }
  if (!rooms_.empty()) {
    RegisterIdentity(own_server_.spec());
  }
  refresh_timer_.Start(FROM_HERE, kRefreshInterval,
                       base::BindRepeating(
                           [](RoomService* self) {
                             for (auto& [id, room] : self->rooms_) {
                               if (room->sync) {
                                 room->sync->Refresh();
                               }
                             }
                           },
                           base::Unretained(this)));
  NotifyRoots();
  NotifyObservers();
}

void RoomService::OnSeedPrefChanged() {
  if (profile_->GetPrefs()->GetString(sync::prefs::kSeed).empty()) {
    // Left sync: rooms stop with the chain (its mounts stay on disk).
    Stop();
  }
}

void RoomService::Stop() {
  for (auto& [room_id, room] : rooms_) {
    room->sync.reset();
    room->holder.reset();
  }
  clients_.clear();
  identity_.reset();
  auth_key_.reset();
  seed_.reset();
  refresh_timer_.Stop();
  NotifyRoots();
  NotifyObservers();
}

void RoomService::ScheduleSave() {
  if (!file_loaded_ || stopped_) {
    return;
  }
  writer_->ScheduleWrite(this);
}

std::optional<std::string> RoomService::SerializeData() {
  return Serialize();
}

std::optional<std::string> RoomService::Serialize() const {
  base::DictValue rooms;
  for (const auto& [room_id, room] : rooms_) {
    base::ListValue keyed;
    for (const std::string& member : room->keyed_members) {
      keyed.Append(member);
    }
    rooms.Set(room_id,
              base::DictValue()
                  .Set("mount",
                       base::Base64Encode(room->mount.SerializeAsString()))
                  .Set("state", room->sync ? room->sync->state().ToValue()
                                           : room->state.ToValue())
                  .Set("keyed", std::move(keyed)));
  }
  base::ListValue notices;
  for (const RemovedNotice& notice : notices_) {
    notices.Append(base::DictValue()
                       .Set("room_id", notice.room_id)
                       .Set("name", notice.name)
                       .Set("kind", notice.kind)
                       .Set("deleted", notice.deleted));
  }
  base::ListValue servers;
  for (const std::string& server : identity_servers_) {
    servers.Append(server);
  }
  base::DictValue root;
  root.Set("rooms", std::move(rooms));
  root.Set("notices", std::move(notices));
  root.Set("identity_servers", std::move(servers));
  return base::WriteJson(root);
}

RoomClient* RoomService::ClientFor(const std::string& server_url) {
  auto it = clients_.find(server_url);
  if (it != clients_.end()) {
    return it->second.get();
  }
  auto client = std::make_unique<RoomClient>(
      GURL(server_url),
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      base::BindRepeating(
          [](base::WeakPtr<RoomService> self) -> std::optional<std::string> {
            return self ? self->MakeToken() : std::nullopt;
          },
          weak_factory_.GetWeakPtr()),
      base::BindRepeating(
          [](base::WeakPtr<RoomService> self) {
            return self ? self->ClientId() : std::string();
          },
          weak_factory_.GetWeakPtr()));
  RoomClient* raw = client.get();
  clients_[server_url] = std::move(client);
  return raw;
}

std::optional<std::string> RoomService::MakeToken() const {
  if (!auth_key_) {
    return std::nullopt;
  }
  return sync::MakeAccessToken(*auth_key_, base::Time::Now());
}

std::string RoomService::ClientId() const {
  syncer::DeviceInfoSyncService* device_info =
      DeviceInfoSyncServiceFactory::GetForProfile(profile_);
  const syncer::DeviceInfo* local =
      device_info
          ? device_info->GetLocalDeviceInfoProvider()->GetLocalDeviceInfo()
          : nullptr;
  return local ? local->guid() : std::string();
}

std::vector<uint8_t> RoomService::MakeProfile(const RoomId& room_id,
                                              uint32_t epoch,
                                              const RoomKey& key) const {
  const std::string json =
      base::WriteJson(base::DictValue().Set("name", GetDisplayName()))
          .value_or("{}");
  std::vector<uint8_t> sealed = sync::rooms::EncryptRecord(
      key, room_id,
      ProfileRecordId(auth_key_ ? sync::GetChainId(*auth_key_) : ""), epoch,
      base::as_byte_span(json));
  std::vector<uint8_t> blob(4);
  base::span(blob).first<4u>().copy_from(base::U32ToBigEndian(epoch));
  blob.insert(blob.end(), sealed.begin(), sealed.end());
  return blob;
}

std::string RoomService::OpenProfile(const Room& room,
                                     const std::vector<uint8_t>& blob,
                                     const std::string& client_id) const {
  if (blob.size() < 4 + sync::rooms::kRecordOverhead) {
    return std::string();
  }
  const uint32_t epoch =
      base::U32FromBigEndian(base::span(blob).first<4u>());
  const std::map<uint32_t, RoomKey> keys =
      room.sync ? room.sync->keys() : KeysOf(room.mount);
  auto key = keys.find(epoch);
  if (key == keys.end()) {
    return std::string();
  }
  std::optional<std::vector<uint8_t>> plain = sync::rooms::DecryptRecord(
      key->second, room.id, ProfileRecordId(client_id), epoch,
      base::span(blob).subspan(4u));
  if (!plain) {
    return std::string();
  }
  std::optional<base::Value> value = base::JSONReader::Read(
      std::string(plain->begin(), plain->end()), base::JSON_PARSE_RFC);
  const std::string* name =
      value && value->is_dict() ? value->GetDict().FindString("name") : nullptr;
  return name ? *name : std::string();
}

RoomService::Room* RoomService::FindRoom(const std::string& room_id) {
  auto it = rooms_.find(room_id);
  return it == rooms_.end() ? nullptr : it->second.get();
}

const RoomService::Room* RoomService::FindRoom(
    const std::string& room_id) const {
  auto it = rooms_.find(room_id);
  return it == rooms_.end() ? nullptr : it->second.get();
}

const RoomService::Room* RoomService::FindRoomByRoot(
    const std::string& root_uuid) const {
  if (root_uuid.empty()) {
    return nullptr;
  }
  for (const auto& [room_id, room] : rooms_) {
    if (room->mount.root_uuid() == root_uuid) {
      return room.get();
    }
  }
  return nullptr;
}

std::string RoomService::FindRoomOf(const std::string& root_uuid) const {
  const Room* room = FindRoomByRoot(root_uuid);
  return room ? room->mount.room_id() : std::string();
}

std::unique_ptr<RoomModel> RoomService::MakeModel(
    const sync_pb::CompassRoomMount& mount) {
  auto placement = base::BindRepeating(&RoomService::PlacementOf,
                                       base::Unretained(this),
                                       mount.room_id());
  if (mount.kind() == "space") {
    return CreateSpaceRoomModel(list_, easels_.get(), mount.root_uuid());
  }
  if (mount.kind() == "easel") {
    return CreateEaselRoomModel(list_, easels_.get(), mount.root_uuid(),
                                sync::RoomTabKey(mount.room_id()),
                                std::move(placement));
  }
  return CreateFolderRoomModel(list_, easels_.get(), mount.root_uuid(),
                               std::move(placement));
}

Placement RoomService::PlacementOf(const std::string& room_id) const {
  Placement placement;
  if (const Room* room = FindRoom(room_id)) {
    placement.space_uuid = room->mount.space_uuid();
    placement.parent_uuid = room->mount.parent_uuid();
  }
  return placement;
}

RoomService::Room& RoomService::Mount(const sync_pb::CompassRoomMount& mount) {
  Room* room = FindRoom(mount.room_id());
  if (!room) {
    auto created = std::make_unique<Room>();
    created->id = ParseRoomId(mount.room_id()).value_or(RoomId{});
    room = created.get();
    rooms_[mount.room_id()] = std::move(created);
    room->mount = mount;
  } else {
    // Keys only grow; the rest is the latest.
    std::map<uint32_t, RoomKey> keys = KeysOf(room->mount);
    keys.merge(KeysOf(mount));
    const std::string root = room->mount.root_uuid();
    room->mount = mount;
    if (room->mount.root_uuid().empty()) {
      room->mount.set_root_uuid(root);
    }
    SetKeys(room->mount, keys);
  }
  StartRoom(*room);
  ScheduleSave();
  return *room;
}

void RoomService::StartRoom(Room& room) {
  if (!IsAvailable() || room.sync) {
    return;
  }
  if (room.mount.root_uuid().empty()) {
    DiscoverRoot(room.mount.room_id());
    return;
  }
  RoomClient* client = ClientFor(room.mount.server_url());
  room.holder = std::make_unique<RoomHolder>(this, room.mount.room_id());
  room.sync = std::make_unique<RoomSync>(
      room.id, room.mount.role(), *identity_, KeysOf(room.mount),
      std::move(room.state),
      MakeRoomClientServer(client, room.mount.room_id()),
      MakeModel(room.mount), room.holder.get());
  room.state = RoomState();
  UpdateExclusions();
  room.sync->Start();
}

void RoomService::Unmount(const std::string& room_id,
                          std::optional<RemovedNotice> notice) {
  auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return;
  }
  VLOG(1) << "[compass-rooms] unmounting " << room_id;
  for (auto& join : it->second->joins) {
    std::move(join).Run(std::string(), l10n_util::GetStringUTF8(
                                           IDS_COMPASS_ROOMS_ERROR_ROOM_GONE));
  }
  rooms_.erase(it);
  if (notice) {
    std::erase_if(notices_, [&](const RemovedNotice& n) {
      return n.room_id == room_id;
    });
    notices_.push_back(std::move(*notice));
  }
  ScheduleSave();
  NotifyRoots();
  NotifyObservers();
}

void RoomService::OnRoomRemoved(const std::string& room_id, bool deleted) {
  const Room* room = FindRoom(room_id);
  if (!room) {
    return;
  }
  RemovedNotice notice{room_id, room->mount.name(), room->mount.kind(),
                       deleted};
  if (const std::string name =
          NameOfItem(list_, easels_ ? easels_->store() : nullptr,
                     room->mount.kind(), room->mount.root_uuid());
      !name.empty()) {
    notice.name = name;
  }
  Unmount(room_id, std::move(notice));
}

void RoomService::OnRoomKeys(const std::string& room_id,
                             const std::map<uint32_t, RoomKey>& keys) {
  Room* room = FindRoom(room_id);
  if (!room) {
    return;
  }
  std::map<uint32_t, RoomKey> all = KeysOf(room->mount);
  for (const auto& [epoch, key] : keys) {
    all[epoch] = key;
  }
  SetKeys(room->mount, all);
  ScheduleSave();
  NotifyLocalChange();
}

void RoomService::OnRoomInfo(const std::string& room_id, const RoomInfo& info) {
  Room* room = FindRoom(room_id);
  if (!room) {
    return;
  }
  const bool role_changed = !info.role.empty() && info.role != room->mount.role();
  // Observers (the share sheet among them) ask for the room again when told
  // it changed, so telling them on every answer would loop.
  const bool changed = !room->info || ShownDiffers(*room->info, info);
  room->info = info;
  if (role_changed) {
    room->mount.set_role(info.role);
    ScheduleSave();
    NotifyLocalChange();
  }
  if (info.role == sync::rooms::kRoleOwner) {
    SendMissingEnvelopes(*room, info);
  }
  if (changed || role_changed) {
    NotifyObservers();
  }
}

void RoomService::SendMissingEnvelopes(Room& room, const RoomInfo& info) {
  if (!auth_key_) {
    return;
  }
  const std::string me = sync::GetChainId(*auth_key_);
  std::vector<RoomClient::MemberEnvelope> envelopes;
  std::vector<std::string> members;
  const std::map<uint32_t, RoomKey> keys = KeysOf(room.mount);
  for (const sync::rooms::RoomMember& member : info.members) {
    if (base::EqualsCaseInsensitiveASCII(member.client_id, me) ||
        room.keyed_members.contains(member.client_id)) {
      continue;
    }
    for (const auto& [epoch, key] : keys) {
      if (epoch > info.epoch) {
        continue;
      }
      std::vector<uint8_t> sealed =
          sync::rooms::SealEnvelope(member.identity_pub, room.id, epoch, key);
      if (!sealed.empty()) {
        envelopes.push_back({member.client_id, epoch, std::move(sealed)});
      }
    }
    members.push_back(member.client_id);
  }
  if (envelopes.empty()) {
    return;
  }
  ClientFor(room.mount.server_url())
      ->AddEnvelopes(
          room.mount.room_id(), envelopes,
          base::BindOnce(
              [](base::WeakPtr<RoomService> self, std::string room_id,
                 std::vector<std::string> members,
                 std::optional<RoomError> error) {
                Room* room = self ? self->FindRoom(room_id) : nullptr;
                if (!room || error) {
                  return;
                }
                room->keyed_members.insert(members.begin(), members.end());
                self->ScheduleSave();
              },
              weak_factory_.GetWeakPtr(), room.mount.room_id(),
              std::move(members)));
}

void RoomService::RegisterIdentity(const std::string& server_url) {
  if (!identity_ || identity_servers_.contains(server_url) ||
      registering_.contains(server_url)) {
    return;
  }
  registering_.insert(server_url);
  ClientFor(server_url)->PutIdentity(
      identity_->public_key,
      base::BindOnce(
          [](base::WeakPtr<RoomService> self, std::string server_url,
             std::optional<RoomError> error) {
            if (!self) {
              return;
            }
            self->registering_.erase(server_url);
            if (!error) {
              self->identity_servers_.insert(server_url);
              self->ScheduleSave();
            }
          },
          weak_factory_.GetWeakPtr(), server_url));
}

void RoomService::DiscoverRoot(const std::string& room_id) {
  Room* room = FindRoom(room_id);
  if (!room || room->discovering || !IsAvailable()) {
    return;
  }
  room->discovering = true;
  ClientFor(room->mount.server_url())
      ->GetChanges(room_id, room->state.seq, RoomSync::kWaitSeconds,
                   base::BindOnce(&RoomService::OnRootRecords,
                                  weak_factory_.GetWeakPtr(), room_id));
}

void RoomService::OnRootRecords(const std::string& room_id,
                                RoomResult<sync::rooms::RoomChanges> result) {
  Room* room = FindRoom(room_id);
  if (!room) {
    return;
  }
  room->discovering = false;
  if (!result.has_value()) {
    if (result.error().is_gone()) {
      OnRoomRemoved(room_id, result.error().status == 404);
      return;
    }
    // Later (the room's owner hasn't written yet, or the network).
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&RoomService::DiscoverRoot, weak_factory_.GetWeakPtr(),
                       room_id),
        base::Seconds(10));
    return;
  }
  const std::map<uint32_t, RoomKey> keys = KeysOf(room->mount);
  std::string root;
  for (const sync::rooms::RoomRecord& record : result->records) {
    room->state.seq = std::max(room->state.seq, record.seq);
    auto key = keys.find(record.epoch);
    if (record.deleted || key == keys.end()) {
      continue;
    }
    std::optional<std::vector<uint8_t>> plain = sync::rooms::DecryptRecord(
        key->second, room->id, record.id, record.epoch, record.ciphertext);
    sync_pb::CompassSpacesSpecifics specifics;
    if (!plain ||
        !specifics.ParseFromArray(plain->data(),
                                  static_cast<int>(plain->size()))) {
      continue;
    }
    const std::string& kind = room->mount.kind();
    if ((kind == "space" && specifics.has_space()) ||
        (kind == "easel" && specifics.has_easel()) ||
        (kind == "folder" && specifics.has_node() &&
         specifics.node().type() == sync::kNodeFolder &&
         specifics.node().parent_uuid().empty())) {
      root = record.id;
      break;
    }
  }
  if (root.empty()) {
    // Not in this page: the next (or the owner's first write).
    DiscoverRoot(room_id);
    return;
  }
  room->mount.set_root_uuid(root);
  // Read from the start: the sync applies every record.
  room->state = RoomState();
  StartRoom(*room);
  ScheduleSave();
  NotifyRoots();
  NotifyObservers();
  for (auto& join : room->joins) {
    std::move(join).Run(root, std::string());
  }
  room->joins.clear();
}

void RoomService::SetSpacesSyncHooks(
    base::RepeatingClosure notify,
    base::RepeatingCallback<void(const std::string&)> apply_waiting) {
  notify_spaces_sync_ = std::move(notify);
  apply_waiting_nodes_ = std::move(apply_waiting);
}

std::optional<std::vector<sync_pb::CompassRoomMount>>
RoomService::GetLocalMounts() const {
  if (!file_loaded_) {
    return std::nullopt;
  }
  std::vector<sync_pb::CompassRoomMount> mounts;
  for (const auto& [room_id, room] : rooms_) {
    mounts.push_back(room->mount);
  }
  return mounts;
}

void RoomService::ApplyRemoteMounts(
    const std::vector<sync_pb::CompassRoomMount>& changed,
    const std::set<std::string>& deleted) {
  for (const std::string& room_id : deleted) {
    // Left on another device of this chain: the item stays here, unshared.
    Unmount(room_id, std::nullopt);
  }
  for (const sync_pb::CompassRoomMount& mount : changed) {
    if (mount.room_id().empty() || !ParseRoomId(mount.room_id())) {
      continue;
    }
    Mount(mount);
  }
  NotifyObservers();
}

sync::RoomRoots RoomService::GetRoomRoots() const {
  sync::RoomRoots roots;
  for (const auto& [room_id, room] : rooms_) {
    const std::string& root = room->mount.root_uuid();
    if (root.empty()) {
      continue;
    }
    if (room->mount.kind() == "space") {
      roots.spaces.insert(root);
    } else if (room->mount.kind() == "easel") {
      roots.easels.insert(root);
      // Its pinned tab is this device's own.
      roots.folders.insert(sync::RoomTabKey(room->mount.room_id()));
    } else {
      roots.folders.insert(root);
    }
    if (room->sync) {
      for (const std::string& easel : room->sync->model().GetEasels()) {
        roots.easels.insert(easel);
      }
    }
  }
  return roots;
}

void RoomService::NotifyRoots() {
  const sync::RoomRoots roots = GetRoomRoots();
  const bool changed = roots.spaces != last_roots_.spaces ||
                       roots.folders != last_roots_.folders ||
                       roots.easels != last_roots_.easels;
  std::set<std::string> new_spaces;
  std::ranges::set_difference(roots.spaces, last_roots_.spaces,
                              std::inserter(new_spaces, new_spaces.end()));
  last_roots_ = roots;
  if (notify_spaces_sync_) {
    // The mounts (and their keys) may have changed too.
    notify_spaces_sync_.Run();
  }
  if (changed && apply_waiting_nodes_) {
    for (const std::string& space : new_spaces) {
      apply_waiting_nodes_.Run(space);
    }
  }
}

void RoomService::UpdateExclusions() {
  for (auto& [room_id, room] : rooms_) {
    if (!room->sync) {
      continue;
    }
    std::set<std::string> others;
    for (const auto& [other_id, other] : rooms_) {
      if (other_id == room_id) {
        continue;
      }
      if (!other->mount.root_uuid().empty()) {
        others.insert(other->mount.root_uuid());
      }
      if (other->mount.kind() == "easel") {
        others.insert(sync::RoomTabKey(other_id));
      }
    }
    room->sync->model().set_excluded(std::move(others));
  }
}

void RoomService::NotifyObservers() {
  for (Observer& observer : observers_) {
    observer.OnRoomsChanged();
  }
}

void RoomService::NotifyLocalChange() {
  if (applying_ || stopped_) {
    return;
  }
  UpdateExclusions();
  bool mounts_changed = false;
  std::vector<std::string> lost;
  for (auto& [room_id, room] : rooms_) {
    if (!room->sync) {
      continue;
    }
    room->sync->NotifyLocalChange();
    // Where a shared folder is is the mount's (synced to this account's
    // other devices).
    if (room->mount.kind() == "folder" && list_) {
      const base::Uuid id = base::Uuid::ParseLowercase(room->mount.root_uuid());
      for (const auto& space : list_->spaces()) {
        SidebarNode* node = space->FindById(id);
        if (!node) {
          continue;
        }
        const std::string space_uuid = space->id().AsLowercaseString();
        const std::string parent_uuid =
            node->parent() && node->parent()->type() != SidebarNode::Type::kRoot
                ? node->parent()->id().AsLowercaseString()
                : std::string();
        if (room->mount.space_uuid() != space_uuid ||
            room->mount.parent_uuid() != parent_uuid) {
          room->mount.set_space_uuid(space_uuid);
          room->mount.set_parent_uuid(parent_uuid);
          mounts_changed = true;
        }
        break;
      }
    }
    if (!room->sync->model().HasRoot() && !room->sync->state().records.empty() &&
        room->mount.kind() == "folder") {
      lost.push_back(room_id);
    }
  }
  for (const std::string& room_id : lost) {
    // Deleted here: it's shared still (the share sheet has Leave and Delete
    // for Everyone), so it comes back, as on iOS.
    Room* room = FindRoom(room_id);
    std::set<std::string> all;
    for (const auto& [key, specifics] : room->sync->state().records) {
      all.insert(key);
    }
    applying_ = true;
    room->sync->model().Apply(room->sync->state().records, all, {});
    applying_ = false;
  }
  if (mounts_changed) {
    ScheduleSave();
  }
  // Easels in a shared folder or space may have come or gone.
  NotifyRoots();
}

void RoomService::OnSpaceAdded(SpaceModel& space) {
  space.AddObserver(this);
  NotifyLocalChange();
}
void RoomService::OnSpaceUpdated(const SpaceModel& space) {
  NotifyLocalChange();
}
void RoomService::OnSpaceWillBeRemoved(SpaceModel& space) {
  space.RemoveObserver(this);
}
void RoomService::OnNodeAdded(const SidebarNode& node) {
  NotifyLocalChange();
}
void RoomService::OnNodeMoved(const SidebarNode& node,
                              const SidebarNode& old_parent,
                              size_t old_index) {
  NotifyLocalChange();
}
void RoomService::OnNodeChanged(const SidebarNode& node) {
  NotifyLocalChange();
}
void RoomService::OnNodeWillBeRemoved(const SidebarNode& node) {
  // After it's gone.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&RoomService::NotifyLocalChange,
                                weak_factory_.GetWeakPtr()));
}
void RoomService::OnSpaceChanged(const SpaceModel& space) {
  NotifyLocalChange();
}
void RoomService::OnEaselsChanged() {
  NotifyLocalChange();
}

std::string RemovedNoticeText(const RemovedNotice& notice) {
  int id;
  if (!notice.name.empty()) {
    id = notice.deleted ? IDS_COMPASS_ROOMS_NOTICE_NAMED_DELETED
                        : IDS_COMPASS_ROOMS_NOTICE_NAMED_REMOVED;
    return l10n_util::GetStringFUTF8(id, base::UTF8ToUTF16(notice.name));
  }
  if (notice.kind == "space") {
    id = notice.deleted ? IDS_COMPASS_ROOMS_NOTICE_SPACE_DELETED
                        : IDS_COMPASS_ROOMS_NOTICE_SPACE_REMOVED;
  } else if (notice.kind == "easel") {
    id = notice.deleted ? IDS_COMPASS_ROOMS_NOTICE_EASEL_DELETED
                        : IDS_COMPASS_ROOMS_NOTICE_EASEL_REMOVED;
  } else {
    id = notice.deleted ? IDS_COMPASS_ROOMS_NOTICE_FOLDER_DELETED
                        : IDS_COMPASS_ROOMS_NOTICE_FOLDER_REMOVED;
  }
  return l10n_util::GetStringUTF8(id);
}

std::string RoomService::TooltipOf(const Room& room) const {
  std::string tooltip = l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_SHARED);
  if (room.info) {
    std::vector<std::string> names;
    for (const sync::rooms::RoomMember& member : room.info->members) {
      std::string name = !member.name.empty()
                             ? member.name
                             : OpenProfile(room, member.profile,
                                           member.client_id);
      if (name.empty()) {
        name = member.email.empty()
                   ? l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_SOMEONE)
                   : member.email;
      }
      names.push_back(name);
    }
    tooltip = l10n_util::GetStringFUTF8(
        IDS_COMPASS_ROOMS_SHARED_WITH,
        base::UTF8ToUTF16(base::JoinString(names, ", ")));
  }
  if (room.mount.role() == sync::rooms::kRoleViewer) {
    tooltip = l10n_util::GetStringFUTF8(IDS_COMPASS_ROOMS_VIEW_ONLY_TOOLTIP,
                                        base::UTF8ToUTF16(tooltip));
  }
  return tooltip;
}

SharedState RoomService::GetNodeState(const SidebarNode& node) const {
  SharedState state;
  for (const auto& [room_id, room] : rooms_) {
    if (!room->sync || !room->sync->model().Contains(node)) {
      continue;
    }
    state.shared = true;
    state.read_only = room->sync->read_only();
    state.tooltip = TooltipOf(*room);
    return state;
  }
  return state;
}

std::map<std::string, SharedState> RoomService::GetSharedRoots() const {
  std::map<std::string, SharedState> roots;
  for (const auto& [room_id, room] : rooms_) {
    if (room->mount.root_uuid().empty()) {
      continue;
    }
    SharedState& state = roots[room->mount.root_uuid()];
    state.shared = true;
    state.read_only = room->mount.role() == sync::rooms::kRoleViewer;
    state.tooltip = TooltipOf(*room);
  }
  return roots;
}

SharedState RoomService::GetSpaceState(const std::string& space_uuid) const {
  SharedState state;
  const Room* room = FindRoomByRoot(space_uuid);
  if (!room || room->mount.kind() != "space") {
    return state;
  }
  state.shared = true;
  state.read_only = room->mount.role() == sync::rooms::kRoleViewer;
  state.tooltip =
      state.read_only
          ? l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_SHARED_VIEW_ONLY)
          : l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_SHARED);
  return state;
}

SharedState RoomService::GetEaselState(const std::string& easel_uuid) const {
  SharedState state;
  for (const auto& [room_id, room] : rooms_) {
    const bool holds =
        room->mount.root_uuid() == easel_uuid ||
        (room->sync && room->sync->model().GetEasels().contains(easel_uuid));
    if (!holds) {
      continue;
    }
    state.shared = true;
    state.read_only = room->mount.role() == sync::rooms::kRoleViewer;
    state.tooltip =
        state.read_only
            ? l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_SHARED_VIEW_ONLY)
            : l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_SHARED);
    return state;
  }
  return state;
}

std::vector<RemovedNotice> RoomService::GetRemovedNotices() const {
  return notices_;
}

void RoomService::DismissRemovedNotice(const std::string& room_id) {
  std::erase_if(notices_, [&](const RemovedNotice& notice) {
    return notice.room_id == room_id;
  });
  ScheduleSave();
  NotifyObservers();
}

void RoomService::Share(
    const std::string& kind,
    const std::string& root_uuid,
    base::OnceCallback<void(const std::string&, const std::string&)> callback) {
  if (!IsAvailable()) {
    std::move(callback).Run(
        std::string(),
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_SHARE_NEEDS_SYNC));
    return;
  }
  if (const Room* room = FindRoomByRoot(root_uuid)) {
    std::move(callback).Run(room->mount.room_id(), std::string());
    return;
  }
  if (!sync::rooms::RoomKindFromName(kind)) {
    std::move(callback).Run(
        std::string(), l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_CANT_SHARE));
    return;
  }
  const std::string name = NameOfItem(
      list_, easels_ ? easels_->store() : nullptr, kind, root_uuid);
  const RoomKey key = sync::rooms::GenerateRoomKey();
  // The client picks the room's ID (POST /rooms "id"), so the first
  // envelope is sealed for it. A server that picks its own gets the
  // envelope again for its ID below.
  const RoomId chosen = sync::rooms::GenerateRoomId();
  const std::vector<uint8_t> envelope = sync::rooms::SealEnvelope(
      identity_->public_key, chosen, 1, key);
  const std::string server = own_server_.spec();
  RoomClient* client = ClientFor(server);
  client->CreateRoom(
      RoomIdText(chosen), kind, identity_->public_key, envelope,
      MakeProfile(chosen, 1, key),
      base::BindOnce(
          [](base::WeakPtr<RoomService> self, std::string kind,
             std::string root_uuid, std::string name, std::string server,
             RoomKey key, std::string chosen_text,
             base::OnceCallback<void(const std::string&, const std::string&)> callback,
             RoomResult<RoomInfo> result) {
            if (!self) {
              return;
            }
            if (!result.has_value()) {
              if (result.error().code == "room_exists") {
                // The ID was taken (vanishingly rare): another one.
                self->Share(kind, root_uuid, std::move(callback));
                return;
              }
              std::move(callback).Run(std::string(), ErrorText(result.error()));
              return;
            }
            std::optional<RoomId> id = ParseRoomId(result->id);
            if (!id) {
              std::move(callback).Run(
                  std::string(),
                  l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ERROR_UNKNOWN));
              return;
            }
            const std::string me = sync::GetChainId(*self->auth_key_);
            if (result->id != chosen_text) {
              // An older server picked the ID: the envelope and profile
              // again, bound to it.
              RoomClient* client = self->ClientFor(server);
              client->AddEnvelopes(
                  result->id,
                  {{me, 1,
                    sync::rooms::SealEnvelope(self->identity_->public_key,
                                              *id, 1, key)}},
                  base::DoNothing());
              client->UpdateMember(result->id, me, std::nullopt,
                                   self->MakeProfile(*id, 1, key),
                                   base::DoNothing());
            }
            sync_pb::CompassRoomMount mount;
            mount.set_room_id(result->id);
            mount.set_server_url(server);
            mount.set_kind(kind);
            mount.set_root_uuid(root_uuid);
            mount.set_role(sync::rooms::kRoleOwner);
            mount.set_name(name);
            SetKeys(mount, {{1u, key}});
            Room& room = self->Mount(mount);
            room.keyed_members.insert(me);
            VLOG(1) << "[compass-rooms] shared " << kind << " " << root_uuid
                    << " as " << result->id;
            self->RegisterIdentity(server);
            // Its placement, and the personal chain lets go of it.
            self->NotifyLocalChange();
            self->NotifyObservers();
            std::move(callback).Run(result->id, std::string());
          },
          weak_factory_.GetWeakPtr(), kind, root_uuid, name, server, key,
          RoomIdText(chosen), std::move(callback)));
}

void RoomService::CreateInviteLink(
    const std::string& room_id,
    const std::string& role,
    base::TimeDelta expires_in,
    int uses,
    base::OnceCallback<void(const std::string&, const std::string&)> callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        std::string(), l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  const std::map<uint32_t, RoomKey> keys =
      room->sync ? room->sync->keys() : KeysOf(room->mount);
  if (keys.empty()) {
    std::move(callback).Run(
        std::string(), l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  const uint32_t epoch =
      room->sync ? std::max(room->sync->epoch(), keys.rbegin()->first)
                 : keys.rbegin()->first;
  auto key = keys.find(epoch);
  if (key == keys.end()) {
    std::move(callback).Run(
        std::string(),
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_TRY_AGAIN_SOON));
    return;
  }
  const sync::rooms::InviteSecret secret = sync::rooms::GenerateInviteSecret();
  sync::rooms::InviteLink link;
  link.server_url = room->mount.server_url();
  link.room_id = room->id;
  link.invite_secret = secret;
  link.epoch_key = key->second;
  link.epoch = epoch;
  std::string name = NameOfItem(list_, easels_ ? easels_->store() : nullptr,
                                room->mount.kind(), room->mount.root_uuid());
  if (name.empty()) {
    name = room->mount.name();
  }
  // At most 256 bytes, cut on a character.
  if (name.size() > sync::rooms::kMaxRoomNameSize) {
    base::TruncateUTF8ToByteSize(name, sync::rooms::kMaxRoomNameSize, &name);
  }
  link.name = name;
  link.kind = *sync::rooms::RoomKindFromName(room->mount.kind());
  const int64_t expires =
      expires_in.is_zero()
          ? 0
          : (base::Time::Now() + expires_in).InMillisecondsSinceUnixEpoch();
  ClientFor(room->mount.server_url())
      ->CreateInvite(
          room_id, role, sync::rooms::InviteSecretHash(secret), expires, uses,
          base::BindOnce(
              [](sync::rooms::InviteLink link,
                 base::OnceCallback<void(const std::string&, const std::string&)> callback,
                 RoomResult<sync::rooms::RoomInvite> result) {
                if (!result.has_value()) {
                  std::move(callback).Run(std::string(),
                                          ErrorText(result.error()));
                  return;
                }
                link.invite_id = result->id;
                std::optional<std::string> text =
                    sync::rooms::EncodeInviteLink(link);
                if (!text) {
                  std::move(callback).Run(std::string(),
                                          l10n_util::GetStringUTF8(
                                              IDS_COMPASS_ROOMS_ERROR_UNKNOWN));
                  return;
                }
                std::move(callback).Run(*text, std::string());
              },
              std::move(link), std::move(callback)));
}

void RoomService::RevokeInvite(const std::string& room_id,
                               const std::string& invite_id,
                               Done callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  ClientFor(room->mount.server_url())
      ->RevokeInvite(room_id, invite_id,
                     base::BindOnce(
                         [](Done callback, std::optional<RoomError> error) {
                           std::move(callback).Run(
                               error ? ErrorText(*error) : std::string());
                         },
                         std::move(callback)));
}

void RoomService::GetRoomView(
    const std::string& room_id,
    base::OnceCallback<void(std::optional<RoomView>, const std::string&)> callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        std::nullopt, l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  RoomClient* client = ClientFor(room->mount.server_url());
  client->GetRoom(
      room_id,
      base::BindOnce(
          [](base::WeakPtr<RoomService> self, std::string room_id,
             base::OnceCallback<void(std::optional<RoomView>, const std::string&)>
                 callback,
             RoomResult<RoomInfo> result) {
            Room* room = self ? self->FindRoom(room_id) : nullptr;
            if (!room) {
              std::move(callback).Run(
                  std::nullopt,
                  l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
              return;
            }
            if (!result.has_value()) {
              if (result.error().is_gone()) {
                self->OnRoomRemoved(room_id, result.error().status == 404);
              }
              std::move(callback).Run(std::nullopt, ErrorText(result.error()));
              return;
            }
            self->OnRoomInfo(room_id, *result);
            RoomView view;
            view.room_id = room_id;
            view.kind = room->mount.kind();
            view.name = NameOfItem(
                self->list_, self->easels_ ? self->easels_->store() : nullptr,
                room->mount.kind(), room->mount.root_uuid());
            if (view.name.empty()) {
              view.name = room->mount.name();
            }
            view.role = result->role;
            view.server = GURL(room->mount.server_url()).host();
            const std::string me = sync::GetChainId(*self->auth_key_);
            for (const sync::rooms::RoomMember& member : result->members) {
              MemberView m;
              m.client_id = member.client_id;
              m.name = !member.name.empty()
                           ? member.name
                           : self->OpenProfile(*room, member.profile, member.client_id);
              m.email = member.email;
              m.role = member.role;
              m.pending = member.pending;
              m.is_me = base::EqualsCaseInsensitiveASCII(member.client_id, me);
              view.members.push_back(std::move(m));
            }
            for (const sync::rooms::RoomInvite& invite : result->invites) {
              view.invites.push_back({invite.id, invite.role, invite.expires,
                                      invite.uses_left.value_or(-1),
                                      invite.revoked});
            }
            // Whether email invites work: the server's sign-in.
            self->ClientFor(room->mount.server_url())
                ->GetInfo(base::BindOnce(
                    [](RoomView view,
                       base::OnceCallback<void(std::optional<RoomView>,
                                               const std::string&)> callback,
                       RoomResult<sync::rooms::ServerInfo> info) {
                      view.oidc = info.has_value() && info->oidc;
                      std::move(callback).Run(std::move(view), std::string());
                    },
                    std::move(view), std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), room_id, std::move(callback)));
}

void RoomService::InviteByEmail(const std::string& room_id,
                                const std::string& email,
                                const std::string& role,
                                Done callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  RoomClient* client = ClientFor(room->mount.server_url());
  client->LookUpDirectory(
      email,
      base::BindOnce(
          [](base::WeakPtr<RoomService> self, std::string room_id,
             std::string role, Done callback,
             RoomResult<std::vector<sync::rooms::DirectoryAccount>> result) {
            Room* room = self ? self->FindRoom(room_id) : nullptr;
            if (!room) {
              std::move(callback).Run(
                  l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
              return;
            }
            if (!result.has_value()) {
              std::move(callback).Run(ErrorText(result.error()));
              return;
            }
            if (result->empty()) {
              std::move(callback).Run(
                  l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NO_SUCH_ACCOUNT));
              return;
            }
            const std::map<uint32_t, RoomKey> keys =
                room->sync ? room->sync->keys() : KeysOf(room->mount);
            std::vector<RoomClient::NewMember> members;
            std::vector<std::string> ids;
            for (const sync::rooms::DirectoryAccount& account : *result) {
              if (room->info && room->info->FindMember(account.client_id)) {
                continue;
              }
              RoomClient::NewMember member;
              member.client_id = account.client_id;
              member.role = role;
              member.identity_pub = account.identity_pub;
              for (const auto& [epoch, key] : keys) {
                member.envelopes.push_back(
                    {epoch, sync::rooms::SealEnvelope(account.identity_pub,
                                                      room->id, epoch, key)});
              }
              ids.push_back(account.client_id);
              members.push_back(std::move(member));
            }
            if (members.empty()) {
              std::move(callback).Run(
                  l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_ALREADY_MEMBER));
              return;
            }
            self->ClientFor(room->mount.server_url())
                ->AddMembers(
                    room_id, members,
                    base::BindOnce(
                        [](base::WeakPtr<RoomService> self,
                           std::string room_id, std::vector<std::string> ids,
                           Done callback, RoomResult<RoomInfo> result) {
                          if (!result.has_value()) {
                            std::move(callback).Run(ErrorText(result.error()));
                            return;
                          }
                          if (Room* room =
                                  self ? self->FindRoom(room_id) : nullptr) {
                            room->keyed_members.insert(ids.begin(), ids.end());
                            self->OnRoomInfo(room_id, *result);
                            self->ScheduleSave();
                          }
                          std::move(callback).Run(std::string());
                        },
                        self, room_id, std::move(ids), std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), room_id, role, std::move(callback)));
}

void RoomService::ChangeRole(const std::string& room_id,
                             const std::string& client_id,
                             const std::string& role,
                             Done callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  ClientFor(room->mount.server_url())
      ->UpdateMember(room_id, client_id, role, std::nullopt,
                     base::BindOnce(
                         [](Done callback, std::optional<RoomError> error) {
                           std::move(callback).Run(
                               error ? ErrorText(*error) : std::string());
                         },
                         std::move(callback)));
}

void RoomService::RemoveMember(const std::string& room_id,
                               const std::string& client_id,
                               Done callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  ClientFor(room->mount.server_url())
      ->RemoveMember(
          room_id, client_id,
          base::BindOnce(
              [](base::WeakPtr<RoomService> self, std::string room_id,
                 std::string client_id, Done callback,
                 std::optional<RoomError> error) {
                if (error) {
                  std::move(callback).Run(ErrorText(*error));
                  return;
                }
                if (!self) {
                  return;
                }
                if (Room* room = self->FindRoom(room_id)) {
                  room->keyed_members.erase(client_id);
                }
                // They had the keys so far: what follows gets a new one.
                self->RotateKey(room_id, std::move(callback));
              },
              weak_factory_.GetWeakPtr(), room_id, client_id,
              std::move(callback)));
}

void RoomService::RotateKey(const std::string& room_id, Done callback) {
  Room* room = FindRoom(room_id);
  if (!room) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  ClientFor(room->mount.server_url())
      ->GetRoom(
          room_id,
          base::BindOnce(
              [](base::WeakPtr<RoomService> self, std::string room_id,
                 Done callback, RoomResult<RoomInfo> result) {
                Room* room = self ? self->FindRoom(room_id) : nullptr;
                if (!room) {
                  std::move(callback).Run(
                      l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
                  return;
                }
                if (!result.has_value()) {
                  std::move(callback).Run(ErrorText(result.error()));
                  return;
                }
                const uint32_t epoch = result->epoch + 1;
                const RoomKey key = sync::rooms::GenerateRoomKey();
                std::map<std::string, std::vector<uint8_t>> envelopes;
                for (const sync::rooms::RoomMember& member :
                     result->members) {
                  envelopes[member.client_id] = sync::rooms::SealEnvelope(
                      member.identity_pub, room->id, epoch, key);
                }
                self->ClientFor(room->mount.server_url())
                    ->Rotate(
                        room_id, epoch, envelopes,
                        base::BindOnce(
                            [](base::WeakPtr<RoomService> self,
                               std::string room_id, uint32_t epoch,
                               RoomKey key, Done callback,
                               std::optional<RoomError> error) {
                              if (error) {
                                std::move(callback).Run(ErrorText(*error));
                                return;
                              }
                              Room* room =
                                  self ? self->FindRoom(room_id) : nullptr;
                              if (room) {
                                self->OnRoomKeys(room_id, {{epoch, key}});
                                if (room->sync) {
                                  room->sync->Refresh();
                                }
                              }
                              VLOG(1) << "[compass-rooms] " << room_id
                                      << " now at epoch " << epoch;
                              std::move(callback).Run(std::string());
                            },
                            self, room_id, epoch, key, std::move(callback)));
              },
              weak_factory_.GetWeakPtr(), room_id, std::move(callback)));
}

void RoomService::Leave(const std::string& room_id, Done callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  ClientFor(room->mount.server_url())
      ->RemoveMember(
          room_id, sync::GetChainId(*auth_key_),
          base::BindOnce(
              [](base::WeakPtr<RoomService> self, std::string room_id,
                 Done callback, std::optional<RoomError> error) {
                if (error && !error->is_gone()) {
                  std::move(callback).Run(ErrorText(*error));
                  return;
                }
                if (self) {
                  self->Unmount(room_id, std::nullopt);
                }
                std::move(callback).Run(std::string());
              },
              weak_factory_.GetWeakPtr(), room_id, std::move(callback)));
}

void RoomService::DeleteRoom(const std::string& room_id, Done callback) {
  Room* room = FindRoom(room_id);
  if (!room || !IsAvailable()) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_NOT_SHARED));
    return;
  }
  ClientFor(room->mount.server_url())
      ->DeleteRoom(
          room_id,
          base::BindOnce(
              [](base::WeakPtr<RoomService> self, std::string room_id,
                 Done callback, std::optional<RoomError> error) {
                if (error && !error->is_gone()) {
                  std::move(callback).Run(ErrorText(*error));
                  return;
                }
                if (self) {
                  self->Unmount(room_id, std::nullopt);
                }
                std::move(callback).Run(std::string());
              },
              weak_factory_.GetWeakPtr(), room_id, std::move(callback)));
}

std::optional<JoinPreview> RoomService::PreviewJoin(
    const std::string& text) const {
  std::optional<sync::rooms::InviteLink> link =
      sync::rooms::ParseInviteLink(text);
  if (!link) {
    return std::nullopt;
  }
  JoinPreview preview;
  preview.name = link->name;
  preview.kind = std::string(sync::rooms::RoomKindName(link->kind));
  preview.server = GURL(link->server_url).host();
  preview.already_joined = FindRoom(RoomIdText(link->room_id));
  return preview;
}

void RoomService::Join(
    const std::string& text,
    const std::string& space_uuid,
    base::OnceCallback<void(const std::string&, const std::string&)> callback) {
  std::optional<sync::rooms::InviteLink> link =
      sync::rooms::ParseInviteLink(text);
  if (!link) {
    std::move(callback).Run(
        std::string(),
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_INVITE_INVALID));
    return;
  }
  if (!IsAvailable()) {
    std::move(callback).Run(
        std::string(),
        l10n_util::GetStringUTF8(IDS_COMPASS_ROOMS_JOIN_NEEDS_SYNC));
    return;
  }
  const std::string room_id = RoomIdText(link->room_id);
  if (Room* room = FindRoom(room_id)) {
    if (room->mount.root_uuid().empty()) {
      room->joins.push_back(std::move(callback));
    } else {
      std::move(callback).Run(room->mount.root_uuid(), std::string());
    }
    return;
  }
  RoomClient* client = ClientFor(link->server_url);
  client->Join(
      room_id, link->invite_id, SecretText(link->invite_secret),
      identity_->public_key,
      MakeProfile(link->room_id, link->epoch, link->epoch_key),
      base::BindOnce(
          [](base::WeakPtr<RoomService> self, sync::rooms::InviteLink link,
             std::string room_id, std::string space_uuid,
             base::OnceCallback<void(const std::string&, const std::string&)> callback,
             RoomResult<RoomInfo> result) {
            if (!self) {
              return;
            }
            if (!result.has_value()) {
              std::move(callback).Run(std::string(), ErrorText(result.error()));
              return;
            }
            std::map<uint32_t, RoomKey> keys = {{link.epoch, link.epoch_key}};
            for (const sync::rooms::RoomEnvelope& envelope :
                 result->envelopes) {
              if (std::optional<RoomKey> key = sync::rooms::OpenEnvelope(
                      *self->identity_, link.room_id, envelope.epoch,
                      envelope.sealed)) {
                keys[envelope.epoch] = *key;
              }
            }
            sync_pb::CompassRoomMount mount;
            mount.set_room_id(room_id);
            mount.set_server_url(link.server_url);
            mount.set_kind(std::string(sync::rooms::RoomKindName(link.kind)));
            mount.set_role(result->role);
            mount.set_name(link.name);
            mount.set_space_uuid(space_uuid);
            SetKeys(mount, keys);
            Room& room = self->Mount(mount);
            room.joins.push_back(std::move(callback));
            VLOG(1) << "[compass-rooms] joined " << room_id << " as "
                    << result->role;
            self->RegisterIdentity(link.server_url);
            self->NotifyObservers();
          },
          weak_factory_.GetWeakPtr(), *link, room_id, space_uuid,
          std::move(callback)));
}

std::string RoomService::GetDisplayName() const {
  std::string name = profile_->GetPrefs()->GetString(kDisplayNamePref);
  if (name.empty()) {
    name = GetOsUserFullName();
  }
  return name;
}

void RoomService::SetDisplayName(const std::string& name) {
  profile_->GetPrefs()->SetString(
      kDisplayNamePref, std::string(base::TrimWhitespaceASCII(
                            name.substr(0, 200), base::TRIM_ALL)));
  if (!IsAvailable()) {
    return;
  }
  // Every room's members see the new one.
  const std::string me = sync::GetChainId(*auth_key_);
  for (auto& [room_id, room] : rooms_) {
    const std::map<uint32_t, RoomKey> keys =
        room->sync ? room->sync->keys() : KeysOf(room->mount);
    if (keys.empty()) {
      continue;
    }
    const auto& [epoch, key] = *keys.rbegin();
    ClientFor(room->mount.server_url())
        ->UpdateMember(room_id, me, std::nullopt,
                       MakeProfile(room->id, epoch, key), base::DoNothing());
  }
}

void RoomService::MoveToNewChain(const sync::Seed& new_seed,
                                 base::OnceClosure done) {
  if (!IsAvailable() || rooms_.empty()) {
    std::move(done).Run();
    return;
  }
  const sync::AuthKey new_auth = sync::DeriveAuthKey(new_seed, account_uuid_);
  const sync::rooms::IdentityKey new_identity =
      sync::rooms::DeriveIdentityKey(new_seed, account_uuid_);
  const std::string linked = sync::MakeAccessToken(new_auth, base::Time::Now());
  base::RepeatingClosure barrier = base::BarrierClosure(
      rooms_.size(),
      base::BindOnce(
          [](base::WeakPtr<RoomService> self, base::OnceClosure done) {
            if (self) {
              // This account's chain is going: the rooms resume with the
              // new one after the restart.
              self->Stop();
            }
            std::move(done).Run();
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
  for (auto& [room_id, room] : rooms_) {
    std::vector<sync::rooms::RoomEnvelope> envelopes;
    const std::map<uint32_t, RoomKey> keys =
        room->sync ? room->sync->keys() : KeysOf(room->mount);
    for (const auto& [epoch, key] : keys) {
      envelopes.push_back({epoch, sync::rooms::SealEnvelope(
                                      new_identity.public_key, room->id, epoch,
                                      key)});
    }
    if (envelopes.empty()) {
      barrier.Run();
      continue;
    }
    ClientFor(room->mount.server_url())
        ->MoveToSuccessor(
            room_id, linked, new_identity.public_key, envelopes,
            base::BindOnce(
                [](std::string room_id, base::RepeatingClosure barrier,
                   std::optional<RoomError> error) {
                  LOG_IF(WARNING, error)
                      << "[compass-rooms] " << room_id
                      << " didn't move to the new chain: " << error->code;
                  VLOG_IF(1, !error) << "[compass-rooms] " << room_id
                                     << " moved to the new chain";
                  barrier.Run();
                },
                room_id, barrier));
  }
}

}  // namespace compass::rooms
