// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ROOMS_ROOM_SERVICE_H_
#define COMPASS_BROWSER_ROOMS_ROOM_SERVICE_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/important_file_writer.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "compass/browser/rooms/room_model.h"
#include "compass/browser/rooms/room_sync.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "compass/components/sync/chain.h"
#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"
#include "compass/components/sync/rooms/room_client.h"
#include "compass/components/sync/rooms/room_crypto.h"
#include "compass/components/sync/rooms/room_invite_link.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"

class Profile;

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace os_crypt_async {
class Encryptor;
}  // namespace os_crypt_async

// Sharing spaces, pinned folders and easels with other people
// (docs/design/sync.md, "Rooms" and "Rooms: clients"). One per regular
// profile (none for Incognito or Guest), on the desktop and Android: the
// profile's sync account (its chain's, or the profile's own in the chain)
// is the member. It owns the account's mounts, which sync through the
// profile's COMPASS_SPACES (CompassSpacesSyncBridge::Delegate), runs each
// mounted room's RoomSync, and does what the share sheet and the join page
// ask.
namespace compass::rooms {

// Prefs.
inline constexpr char kDisplayNamePref[] = "compass.rooms.display_name";

// What the share sheet shows of a room.
struct MemberView {
  std::string client_id;
  // Company sign-in's name, or the member's own (their profile), or "".
  std::string name;
  std::string email;
  std::string role;
  bool pending = false;
  bool is_me = false;
};

struct InviteView {
  std::string id;
  std::string role;
  int64_t expires = 0;   // Unix ms, 0 never.
  int uses_left = -1;    // -1 unlimited.
  bool revoked = false;
};

struct RoomView {
  RoomView();
  RoomView(const RoomView&);
  RoomView& operator=(const RoomView&);
  ~RoomView();

  std::string room_id;
  std::string kind;
  std::string name;
  std::string role;
  std::string server;
  // Company sign-in on the room's server: email invites work.
  bool oidc = false;
  std::vector<MemberView> members;
  std::vector<InviteView> invites;
};

// What an invite link says, before joining.
struct JoinPreview {
  std::string name;
  std::string kind;
  std::string server;
  // Already a member here (the mount exists).
  bool already_joined = false;
};

// A room this account was removed from, or that was deleted: the sidebar
// shows a banner until dismissed. The item stays, unshared.
struct RemovedNotice {
  std::string room_id;
  std::string name;
  std::string kind;
  bool deleted = false;
};

// The model's state for a sidebar node or space.
struct SharedState {
  bool shared = false;
  bool read_only = false;
  // The members' names, for the badge's tooltip.
  std::string tooltip;
};

class RoomService : public KeyedService,
                    public SpaceList::Observer,
                    public SpaceModel::Observer,
                    public base::ImportantFileWriter::DataSerializer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Mounts, roles, members or notices changed.
    virtual void OnRoomsChanged() {}
  };

  using Done = base::OnceCallback<void(const std::string& error)>;

  // Null for profiles that don't share (Incognito, Guest).
  static RoomService* Get(Profile* profile);
  static RoomService* GetIfExists(Profile* profile);
  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);
  static void EnsureFactoryBuilt();

  explicit RoomService(Profile* profile);
  RoomService(const RoomService&) = delete;
  RoomService& operator=(const RoomService&) = delete;
  ~RoomService() override;

  // KeyedService:
  void Shutdown() override;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);
  base::WeakPtr<RoomService> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // Whether sharing can work: the profile syncs (its seed is read) and the
  // mounts are loaded.
  bool IsAvailable() const;

  // The personal chain's side (CompassSpacesSyncBridge::Delegate).
  std::optional<std::vector<sync_pb::CompassRoomMount>> GetLocalMounts() const;
  void ApplyRemoteMounts(const std::vector<sync_pb::CompassRoomMount>& changed,
                         const std::set<std::string>& deleted);
  sync::RoomRoots GetRoomRoots() const;
  // The bridge's hooks: a pass looks again (mounts or what they cover
  // changed), and nodes waiting for a space a room brought apply.
  void SetSpacesSyncHooks(
      base::RepeatingClosure notify,
      base::RepeatingCallback<void(const std::string&)> apply_waiting);

  // The sidebar's view.
  SharedState GetNodeState(const SidebarNode& node) const;
  SharedState GetSpaceState(const std::string& space_uuid) const;
  SharedState GetEaselState(const std::string& easel_uuid) const;
  // Whether |node| (or a space, an easel) may be changed here: false for a
  // viewer's shared items.
  bool CanEdit(const SidebarNode& node) const {
    return !GetNodeState(node).read_only;
  }
  // Every shared item's UUID (space, folder, easel board) and its state.
  std::map<std::string, SharedState> GetSharedRoots() const;
  // The room of a shared space, folder or easel, by its UUID; "" if none.
  std::string FindRoomOf(const std::string& root_uuid) const;
  std::vector<RemovedNotice> GetRemovedNotices() const;
  void DismissRemovedNotice(const std::string& room_id);

  // Sharing. |kind| "space", "folder" or "easel"; |root_uuid| the item.
  // Answers the room's ID, or an error to show.
  void Share(const std::string& kind,
             const std::string& root_uuid,
             base::OnceCallback<void(const std::string& room_id, const std::string& error)>
                 callback);
  // A new invite link: |role| editor or viewer, |expires_in| zero for never,
  // |uses| 0 for unlimited.
  void CreateInviteLink(
      const std::string& room_id,
      const std::string& role,
      base::TimeDelta expires_in,
      int uses,
      base::OnceCallback<void(const std::string& link, const std::string& error)> callback);
  void RevokeInvite(const std::string& room_id,
                    const std::string& invite_id,
                    Done callback);
  void GetRoomView(
      const std::string& room_id,
      base::OnceCallback<void(std::optional<RoomView>, const std::string& error)>
          callback);
  // Company sign-in servers: invites |email|'s accounts.
  void InviteByEmail(const std::string& room_id,
                     const std::string& email,
                     const std::string& role,
                     Done callback);
  void ChangeRole(const std::string& room_id,
                  const std::string& client_id,
                  const std::string& role,
                  Done callback);
  // Removes a member and rotates the key, so they can't read what follows.
  void RemoveMember(const std::string& room_id,
                    const std::string& client_id,
                    Done callback);
  // Leaves (a member) or deletes the room for everyone (the owner). The item
  // stays here, unshared.
  void Leave(const std::string& room_id, Done callback);
  void DeleteRoom(const std::string& room_id, Done callback);

  // Joining.
  std::optional<JoinPreview> PreviewJoin(const std::string& link) const;
  // Joins the room of |link|. A folder or easel goes in |space_uuid| (""
  // for the active space).
  void Join(const std::string& link,
            const std::string& space_uuid,
            base::OnceCallback<void(const std::string& root_uuid, const std::string& error)>
                callback);

  // "Your name for sharing": what other members see without company
  // sign-in. Defaults to the computer's user name.
  std::string GetDisplayName() const;
  void SetDisplayName(const std::string& name);

  // Reset sync (CompassSyncService::ResetChain()): moves every membership to
  // the new chain's account before the old chain goes, then stops.
  void MoveToNewChain(const sync::Seed& new_seed, base::OnceClosure done);

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;
  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeChanged(const SidebarNode& node) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;
  void OnSpaceChanged(const SpaceModel& space) override;

  // Easels changed (EaselStore::Observer, through an adapter).
  void OnEaselsChanged();

  // base::ImportantFileWriter::DataSerializer:
  std::optional<std::string> SerializeData() override;

 private:
  class EaselSourceImpl;
  struct Room;
  class RoomHolder;

  void OnEncryptor(scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void OnSpacesLoaded();
  void OnFileRead(std::optional<std::string> contents);
  void MaybeStart();
  void OnSeedPrefChanged();
  void Stop();
  void ScheduleSave();
  std::optional<std::string> Serialize() const;

  sync::rooms::RoomClient* ClientFor(const std::string& server_url);
  std::optional<std::string> MakeToken() const;
  std::string ClientId() const;
  // Sealed name for a room's members, of |epoch|.
  std::vector<uint8_t> MakeProfile(const sync::rooms::RoomId& room_id,
                                   uint32_t epoch,
                                   const sync::rooms::RoomKey& key) const;
  std::string OpenProfile(const Room& room,
                          const std::vector<uint8_t>& blob,
                          const std::string& client_id) const;

  // "Shared with …" for a room's badge.
  std::string TooltipOf(const Room& room) const;
  Room* FindRoom(const std::string& room_id);
  const Room* FindRoom(const std::string& room_id) const;
  const Room* FindRoomByRoot(const std::string& root_uuid) const;
  // Adds or updates the mount; starts its sync if it can.
  Room& Mount(const sync_pb::CompassRoomMount& mount);
  void StartRoom(Room& room);
  void Unmount(const std::string& room_id,
               std::optional<RemovedNotice> notice);
  void NotifyRoots();
  // Each room's model leaves out the other rooms' items inside it.
  void UpdateExclusions();
  void NotifyLocalChange();
  void NotifyObservers();
  void RegisterIdentity(const std::string& server_url);
  // A joined room whose root isn't known yet (a link gives none): its first
  // records say.
  void DiscoverRoot(const std::string& room_id);
  void OnRootRecords(const std::string& room_id,
                     sync::rooms::RoomResult<sync::rooms::RoomChanges> result);
  void RotateKey(const std::string& room_id, Done callback);
  void SendMissingEnvelopes(Room& room, const sync::rooms::RoomInfo& info);
  void OnRoomRemoved(const std::string& room_id, bool deleted);
  void OnRoomKeys(const std::string& room_id,
                  const std::map<uint32_t, sync::rooms::RoomKey>& keys);
  void OnRoomInfo(const std::string& room_id,
                  const sync::rooms::RoomInfo& info);
  Placement PlacementOf(const std::string& room_id) const;
  std::unique_ptr<RoomModel> MakeModel(const sync_pb::CompassRoomMount& mount);

  const raw_ptr<Profile> profile_;
  raw_ptr<SpaceList> list_ = nullptr;
  std::unique_ptr<EaselSourceImpl> easels_;
  PrefChangeRegistrar pref_change_registrar_;
  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  std::unique_ptr<base::ImportantFileWriter> writer_;

  std::optional<sync::Seed> seed_;
  std::string account_uuid_;
  std::optional<sync::AuthKey> auth_key_;
  std::optional<sync::rooms::IdentityKey> identity_;
  GURL own_server_;
  bool spaces_loaded_ = false;
  bool file_loaded_ = false;
  bool stopped_ = false;

  std::map<std::string, std::unique_ptr<sync::rooms::RoomClient>> clients_;
  std::map<std::string, std::unique_ptr<Room>> rooms_;
  std::vector<RemovedNotice> notices_;
  // Servers this account's identity key was registered with.
  std::set<std::string> identity_servers_;
  std::set<std::string> registering_;

  base::RepeatingClosure notify_spaces_sync_;
  base::RepeatingCallback<void(const std::string&)> apply_waiting_nodes_;
  sync::RoomRoots last_roots_;
  bool applying_ = false;
  base::RepeatingTimer refresh_timer_;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<RoomService> weak_factory_{this};
};

}  // namespace compass::rooms

#endif  // COMPASS_BROWSER_ROOMS_ROOM_SERVICE_H_
