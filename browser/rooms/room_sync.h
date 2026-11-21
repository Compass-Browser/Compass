// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ROOMS_ROOM_SYNC_H_
#define COMPASS_BROWSER_ROOMS_ROOM_SYNC_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "compass/browser/rooms/room_model.h"
#include "compass/components/sync/rooms/room_client.h"
#include "compass/components/sync/rooms/room_crypto.h"

// Syncs one room (docs/design/sync.md, "Rooms: clients"): reads the changes
// feed (a long poll), decrypts each record with its epoch's key and applies
// it to the shared item through a RoomModel; and diffs the model against
// the room as last synced (the mirror, as the spaces bridge does) and writes
// what changed, encrypted with the current epoch's key, each based on the
// seq it was read at. Desktop and Android.
namespace compass::rooms {

// The routes RoomSync uses, so tests can fake the server
// (RoomClientServer below forwards to a RoomClient).
class RoomServer {
 public:
  virtual ~RoomServer() = default;
  virtual void GetRoom(sync::rooms::RoomClient::Callback<
                       sync::rooms::RoomInfo> callback) = 0;
  virtual void GetChanges(int64_t since,
                          int wait_seconds,
                          sync::rooms::RoomClient::Callback<
                              sync::rooms::RoomChanges> callback) = 0;
  virtual void Commit(const std::vector<sync::rooms::RecordWrite>& writes,
                      sync::rooms::RoomClient::Callback<
                          sync::rooms::CommitResult> callback) = 0;
};

// A RoomServer on a RoomClient (which must outlive it).
std::unique_ptr<RoomServer> MakeRoomClientServer(
    sync::rooms::RoomClient* client,
    std::string room_id);

// What a room has synced: the cursor and every record as last read or
// written (seq and plaintext), kept on disk between runs.
struct RoomState {
  RoomState();
  RoomState(const RoomState&);
  RoomState& operator=(const RoomState&);
  RoomState(RoomState&&);
  RoomState& operator=(RoomState&&);
  ~RoomState();

  base::DictValue ToValue() const;
  static RoomState FromValue(const base::DictValue& value);

  int64_t seq = 0;
  Records records;
  std::map<std::string, int64_t> record_seqs;
};

class RoomSync {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // Keys of epochs this account didn't have came (from envelopes): the
    // mount keeps them.
    virtual void OnKeys(const std::map<uint32_t, sync::rooms::RoomKey>& keys) {}
    // The room as the server last answered it (members, role, invites).
    virtual void OnRoom(const sync::rooms::RoomInfo& room) {}
    // The state changed: save it (debounced by the caller).
    virtual void OnStateChanged() {}
    // The account is no longer a member (removed, or left elsewhere), or
    // the room was deleted. Sync stopped.
    virtual void OnRemoved(bool room_deleted) {}
    // A write was refused for good (too large, quota): shown to the user.
    virtual void OnWriteRefused(const sync::rooms::RoomError& error) {}
  };

  // Long polls hold this long (s).
  static constexpr int kWaitSeconds = 25;
  // Local edits are written this long after the last one.
  static constexpr base::TimeDelta kSendDelay = base::Milliseconds(800);
  // Writes per request (the server takes 500).
  static constexpr size_t kMaxWritesPerCommit = 200;
  // About this much ciphertext per request (the server takes 16 MiB).
  static constexpr size_t kMaxBytesPerCommit = 8 * 1024 * 1024;

  RoomSync(const sync::rooms::RoomId& room_id,
           std::string role,
           const sync::rooms::IdentityKey& identity,
           std::map<uint32_t, sync::rooms::RoomKey> keys,
           RoomState state,
           std::unique_ptr<RoomServer> server,
           std::unique_ptr<RoomModel> model,
           Delegate* delegate);
  RoomSync(const RoomSync&) = delete;
  RoomSync& operator=(const RoomSync&) = delete;
  ~RoomSync();

  // Reads the room (envelopes), then follows its changes.
  void Start();

  // The model changed: a pass looks for what to write (a viewer's edits
  // are undone instead).
  void NotifyLocalChange();

  // A room event, or the app came back: reads the room again (members,
  // keys) and the changes at once.
  void Refresh();

  const std::string& role() const { return role_; }
  bool read_only() const { return role_ == sync::rooms::kRoleViewer; }
  const RoomState& state() const { return state_; }
  RoomModel& model() { return *model_; }
  const std::map<uint32_t, sync::rooms::RoomKey>& keys() const {
    return keys_;
  }
  uint32_t epoch() const { return epoch_; }
  bool stopped() const { return stopped_; }

  // For tests: the pass now, and whether requests are out.
  void SendLocalChangesForTesting() { SendLocalChanges(); }
  bool idle_for_testing() const {
    return !poll_in_flight_ && !commit_in_flight_ && !room_in_flight_;
  }

 private:
  void FetchRoom();
  void OnRoom(sync::rooms::RoomResult<sync::rooms::RoomInfo> result);
  void Poll();
  void OnChanges(int64_t since,
                 sync::rooms::RoomResult<sync::rooms::RoomChanges> result);
  // Decrypts |record| into |out|; false if its epoch's key isn't here.
  bool Decrypt(const sync::rooms::RoomRecord& record,
               sync_pb::CompassSpacesSpecifics& out) const;
  void ScheduleSend();
  void SendLocalChanges();
  void OnCommitted(Records sent,
                   std::set<std::string> deleted,
                   sync::rooms::RoomResult<sync::rooms::CommitResult> result);
  // Undoes the model's changes to what the room has (a viewer's).
  void Revert(const Records& local);
  void Fail(const sync::rooms::RoomError& error);
  void RetryLater(base::OnceClosure what);

  const sync::rooms::RoomId room_id_;
  std::string role_;
  const sync::rooms::IdentityKey identity_;
  std::map<uint32_t, sync::rooms::RoomKey> keys_;
  uint32_t epoch_ = 0;
  RoomState state_;
  std::unique_ptr<RoomServer> server_;
  std::unique_ptr<RoomModel> model_;
  const raw_ptr<Delegate> delegate_;

  bool started_ = false;
  bool stopped_ = false;
  bool poll_in_flight_ = false;
  bool commit_in_flight_ = false;
  bool room_in_flight_ = false;
  bool send_again_ = false;
  bool applying_ = false;
  // Conflicts in a row; past a few, the pass waits.
  int conflicts_ = 0;
  int failures_ = 0;
  // The cursor when the room was last read for a missing key.
  int64_t keys_fetched_at_seq_ = -1;
  base::OneShotTimer send_timer_;
  base::OneShotTimer retry_timer_;
  base::WeakPtrFactory<RoomSync> weak_factory_{this};
};

}  // namespace compass::rooms

#endif  // COMPASS_BROWSER_ROOMS_ROOM_SYNC_H_
