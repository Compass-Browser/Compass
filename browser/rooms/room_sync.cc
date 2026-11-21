// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/room_sync.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"

namespace compass::rooms {

namespace {

using sync::rooms::CommitResult;
using sync::rooms::RecordWrite;
using sync::rooms::RoomChanges;
using sync::rooms::RoomClient;
using sync::rooms::RoomError;
using sync::rooms::RoomInfo;
using sync::rooms::RoomKey;
using sync::rooms::RoomRecord;
using sync::rooms::RoomResult;

constexpr base::TimeDelta kMaxRetryDelay = base::Minutes(5);
// Conflicts in a row before a pass waits for the next change.
constexpr int kMaxConflicts = 4;

class RoomClientServer : public RoomServer {
 public:
  RoomClientServer(RoomClient* client, std::string room_id)
      : client_(client), room_id_(std::move(room_id)) {}

  void GetRoom(RoomClient::Callback<RoomInfo> callback) override {
    client_->GetRoom(room_id_, std::move(callback));
  }
  void GetChanges(int64_t since,
                  int wait_seconds,
                  RoomClient::Callback<RoomChanges> callback) override {
    client_->GetChanges(room_id_, since, wait_seconds, std::move(callback));
  }
  void Commit(const std::vector<RecordWrite>& writes,
              RoomClient::Callback<CommitResult> callback) override {
    client_->CommitRecords(room_id_, writes, std::move(callback));
  }

 private:
  const raw_ptr<RoomClient> client_;
  const std::string room_id_;
};

bool SameContent(const sync_pb::CompassSpacesSpecifics& a,
                 const sync_pb::CompassSpacesSpecifics& b) {
  // An image never changes: present is the same.
  if (a.has_easel_asset() && b.has_easel_asset()) {
    return a.easel_asset().sha256() == b.easel_asset().sha256();
  }
  return a.SerializeAsString() == b.SerializeAsString();
}

}  // namespace

std::unique_ptr<RoomServer> MakeRoomClientServer(RoomClient* client,
                                                  std::string room_id) {
  return std::make_unique<RoomClientServer>(client, std::move(room_id));
}

RoomState::RoomState() = default;
RoomState::RoomState(const RoomState&) = default;
RoomState& RoomState::operator=(const RoomState&) = default;
RoomState::RoomState(RoomState&&) = default;
RoomState& RoomState::operator=(RoomState&&) = default;
RoomState::~RoomState() = default;

base::DictValue RoomState::ToValue() const {
  base::DictValue records_value;
  for (const auto& [id, record_seq] : record_seqs) {
    base::DictValue record;
    record.Set("seq", base::NumberToString(record_seq));
    if (auto it = records.find(id); it != records.end()) {
      record.Set("data", base::Base64Encode(it->second.SerializeAsString()));
    }
    records_value.Set(id, std::move(record));
  }
  base::DictValue value;
  value.Set("seq", base::NumberToString(seq));
  value.Set("records", std::move(records_value));
  return value;
}

// static
RoomState RoomState::FromValue(const base::DictValue& value) {
  RoomState state;
  if (const std::string* seq = value.FindString("seq")) {
    base::StringToInt64(*seq, &state.seq);
  }
  if (const base::DictValue* records = value.FindDict("records")) {
    for (const auto [id, entry] : *records) {
      if (!entry.is_dict()) {
        continue;
      }
      int64_t seq = 0;
      if (const std::string* text = entry.GetDict().FindString("seq")) {
        base::StringToInt64(*text, &seq);
      }
      state.record_seqs[id] = seq;
      if (const std::string* data = entry.GetDict().FindString("data")) {
        std::string bytes;
        sync_pb::CompassSpacesSpecifics specifics;
        if (base::Base64Decode(*data, &bytes) &&
            specifics.ParseFromString(bytes)) {
          state.records[id] = std::move(specifics);
        }
      }
    }
  }
  return state;
}

RoomSync::RoomSync(const sync::rooms::RoomId& room_id,
                   std::string role,
                   const sync::rooms::IdentityKey& identity,
                   std::map<uint32_t, RoomKey> keys,
                   RoomState state,
                   std::unique_ptr<RoomServer> server,
                   std::unique_ptr<RoomModel> model,
                   Delegate* delegate)
    : room_id_(room_id),
      role_(std::move(role)),
      identity_(identity),
      keys_(std::move(keys)),
      state_(std::move(state)),
      server_(std::move(server)),
      model_(std::move(model)),
      delegate_(delegate) {
  if (!keys_.empty()) {
    epoch_ = keys_.rbegin()->first;
  }
}

RoomSync::~RoomSync() = default;

void RoomSync::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  // What the model has from before (and a room's first records, for one
  // this device created) is applied as the room has it, or written.
  FetchRoom();
}

void RoomSync::NotifyLocalChange() {
  if (applying_ || stopped_) {
    return;
  }
  ScheduleSend();
}

void RoomSync::Refresh() {
  if (!started_ || stopped_) {
    return;
  }
  retry_timer_.Stop();
  FetchRoom();
}

void RoomSync::FetchRoom() {
  if (room_in_flight_ || stopped_) {
    return;
  }
  room_in_flight_ = true;
  server_->GetRoom(
      base::BindOnce(&RoomSync::OnRoom, weak_factory_.GetWeakPtr()));
}

void RoomSync::OnRoom(RoomResult<RoomInfo> result) {
  room_in_flight_ = false;
  if (!result.has_value()) {
    if (result.error().is_gone()) {
      Fail(result.error());
      return;
    }
    RetryLater(base::BindOnce(&RoomSync::FetchRoom,
                              weak_factory_.GetWeakPtr()));
    return;
  }
  failures_ = 0;
  const RoomInfo& room = *result;
  if (!room.role.empty()) {
    role_ = room.role;
  }
  epoch_ = std::max(epoch_, room.epoch);
  std::map<uint32_t, RoomKey> added;
  for (const sync::rooms::RoomEnvelope& envelope : room.envelopes) {
    if (keys_.contains(envelope.epoch)) {
      continue;
    }
    if (std::optional<RoomKey> key = sync::rooms::OpenEnvelope(
            identity_, room_id_, envelope.epoch, envelope.sealed)) {
      keys_[envelope.epoch] = *key;
      added[envelope.epoch] = *key;
    } else {
      LOG(WARNING) << "[compass-rooms] an envelope doesn't open (epoch "
                   << envelope.epoch << ")";
    }
  }
  if (!added.empty()) {
    delegate_->OnKeys(added);
  }
  delegate_->OnRoom(room);
  if (room.pending) {
    // An email invite not joined yet: nothing to read.
    return;
  }
  Poll();
  ScheduleSend();
}

void RoomSync::Poll() {
  if (poll_in_flight_ || stopped_) {
    return;
  }
  poll_in_flight_ = true;
  const int64_t since = state_.seq;
  server_->GetChanges(
      since, kWaitSeconds,
      base::BindOnce(&RoomSync::OnChanges, weak_factory_.GetWeakPtr(), since));
}

bool RoomSync::Decrypt(const RoomRecord& record,
                       sync_pb::CompassSpacesSpecifics& out) const {
  auto key = keys_.find(record.epoch);
  if (key == keys_.end()) {
    return false;
  }
  std::optional<std::vector<uint8_t>> plain = sync::rooms::DecryptRecord(
      key->second, room_id_, record.id, record.epoch, record.ciphertext);
  if (!plain) {
    // Tampered with, or not for this room: skipped, as unreadable.
    LOG(WARNING) << "[compass-rooms] record " << record.id
                 << " doesn't decrypt; skipped";
    out.Clear();
    return true;
  }
  if (!out.ParseFromArray(plain->data(), static_cast<int>(plain->size()))) {
    out.Clear();
  }
  return true;
}

void RoomSync::OnChanges(int64_t since, RoomResult<RoomChanges> result) {
  poll_in_flight_ = false;
  if (stopped_) {
    return;
  }
  if (!result.has_value()) {
    if (result.error().is_gone()) {
      Fail(result.error());
      return;
    }
    RetryLater(base::BindOnce(&RoomSync::Poll, weak_factory_.GetWeakPtr()));
    return;
  }
  failures_ = 0;
  const RoomChanges& changes = *result;
  if (changes.epoch > epoch_) {
    epoch_ = changes.epoch;
  }
  std::set<std::string> changed;
  Records deleted;
  bool need_keys = false;
  for (const RoomRecord& record : changes.records) {
    if (auto known = state_.record_seqs.find(record.id);
        known != state_.record_seqs.end() && record.seq <= known->second) {
      // Our own write, or one read before.
      state_.seq = std::max(state_.seq, record.seq);
      continue;
    }
    if (record.deleted) {
      if (auto it = state_.records.find(record.id);
          it != state_.records.end()) {
        deleted[record.id] = std::move(it->second);
        state_.records.erase(it);
      }
      changed.erase(record.id);
      state_.record_seqs[record.id] = record.seq;
      state_.seq = std::max(state_.seq, record.seq);
      continue;
    }
    sync_pb::CompassSpacesSpecifics specifics;
    if (!Decrypt(record, specifics)) {
      need_keys = true;
      break;
    }
    state_.record_seqs[record.id] = record.seq;
    state_.seq = std::max(state_.seq, record.seq);
    if (specifics.ByteSizeLong() == 0) {
      continue;
    }
    auto it = state_.records.find(record.id);
    if (it != state_.records.end() && SameContent(it->second, specifics)) {
      continue;
    }
    state_.records[record.id] = std::move(specifics);
    changed.insert(record.id);
    deleted.erase(record.id);
  }
  if (!changed.empty() || !deleted.empty()) {
    VLOG(1) << "[compass-rooms] applying " << changed.size() << " changed, "
            << deleted.size() << " deleted";
    applying_ = true;
    model_->Apply(state_.records, changed, deleted);
    applying_ = false;
  }
  if (state_.seq != since || !changed.empty() || !deleted.empty()) {
    delegate_->OnStateChanged();
  }
  if (need_keys) {
    // A newer epoch than the keys here (a rotation): the envelope first.
    if (state_.seq == since && keys_fetched_at_seq_ == since) {
      // The room gave no key for it: this account can't read it (joined
      // after the rotation by an old link; the owner adds it). Later.
      RetryLater(base::BindOnce(&RoomSync::FetchRoom,
                                weak_factory_.GetWeakPtr()));
      return;
    }
    keys_fetched_at_seq_ = state_.seq;
    FetchRoom();
    return;
  }
  // A model the room just filled may need fix-ups written; an editor's
  // local changes made meanwhile too.
  if (!changed.empty() || !deleted.empty()) {
    ScheduleSend();
  }
  Poll();
}

void RoomSync::ScheduleSend() {
  if (stopped_ || !started_) {
    return;
  }
  send_timer_.Start(FROM_HERE, kSendDelay,
                    base::BindOnce(&RoomSync::SendLocalChanges,
                                   weak_factory_.GetWeakPtr()));
}

void RoomSync::SendLocalChanges() {
  if (stopped_ || !started_) {
    return;
  }
  if (commit_in_flight_ || room_in_flight_) {
    send_again_ = true;
    return;
  }
  send_again_ = false;
  std::optional<Records> local = model_->GetLocal(state_.records);
  if (!local) {
    return;
  }
  if (read_only()) {
    Revert(*local);
    return;
  }
  auto key = keys_.find(epoch_);
  if (key == keys_.end()) {
    FetchRoom();
    return;
  }
  std::vector<RecordWrite> writes;
  Records sent;
  std::set<std::string> deleted;
  size_t bytes = 0;
  bool more = false;
  auto full = [&] {
    return writes.size() >= kMaxWritesPerCommit || bytes >= kMaxBytesPerCommit;
  };
  for (const auto& [id, specifics] : *local) {
    auto it = state_.records.find(id);
    if (it != state_.records.end() && SameContent(it->second, specifics)) {
      continue;
    }
    if (full()) {
      more = true;
      break;
    }
    const std::string plain = specifics.SerializeAsString();
    RecordWrite write;
    write.id = id;
    write.epoch = epoch_;
    write.ciphertext = sync::rooms::EncryptRecord(
        key->second, room_id_, id, epoch_, base::as_byte_span(plain));
    auto seq = state_.record_seqs.find(id);
    write.base_seq = seq != state_.record_seqs.end() &&
                             state_.records.contains(id)
                         ? seq->second
                         : 0;
    bytes += write.ciphertext.size();
    writes.push_back(std::move(write));
    sent[id] = specifics;
  }
  for (const auto& [id, specifics] : state_.records) {
    if (local->contains(id)) {
      continue;
    }
    if (full()) {
      more = true;
      break;
    }
    RecordWrite write;
    write.id = id;
    write.epoch = epoch_;
    write.deleted = true;
    auto seq = state_.record_seqs.find(id);
    write.base_seq = seq != state_.record_seqs.end() ? seq->second : 0;
    writes.push_back(std::move(write));
    deleted.insert(id);
  }
  if (writes.empty()) {
    return;
  }
  VLOG(1) << "[compass-rooms] writing " << sent.size() << " changed, "
          << deleted.size() << " deleted";
  send_again_ = more;
  commit_in_flight_ = true;
  server_->Commit(writes, base::BindOnce(&RoomSync::OnCommitted,
                                         weak_factory_.GetWeakPtr(),
                                         std::move(sent), std::move(deleted)));
}

void RoomSync::OnCommitted(Records sent,
                           std::set<std::string> deleted,
                           RoomResult<CommitResult> result) {
  commit_in_flight_ = false;
  if (stopped_) {
    return;
  }
  if (result.has_value()) {
    conflicts_ = 0;
    failures_ = 0;
    for (auto& [id, specifics] : sent) {
      state_.records[id] = std::move(specifics);
      if (auto seq = result->records.find(id); seq != result->records.end()) {
        state_.record_seqs[id] = seq->second;
      }
    }
    for (const std::string& id : deleted) {
      state_.records.erase(id);
      if (auto seq = result->records.find(id); seq != result->records.end()) {
        state_.record_seqs[id] = seq->second;
      }
    }
    delegate_->OnStateChanged();
    if (send_again_) {
      SendLocalChanges();
    }
    return;
  }
  const RoomError& error = result.error();
  if (error.is_conflict()) {
    // Another member wrote some of these since we read them: theirs win (as
    // on iOS). Their state goes into the mirror and the model; the rest of
    // ours goes again.
    std::set<std::string> changed;
    Records gone;
    for (const RoomRecord& current : error.current) {
      sync_pb::CompassSpacesSpecifics specifics;
      if (current.seq == 0 || current.deleted ||
          !Decrypt(current, specifics) || specifics.ByteSizeLong() == 0) {
        if (auto it = state_.records.find(current.id);
            it != state_.records.end()) {
          gone[current.id] = std::move(it->second);
          state_.records.erase(it);
        }
        if (current.seq == 0) {
          state_.record_seqs.erase(current.id);
        } else {
          state_.record_seqs[current.id] = current.seq;
        }
        continue;
      }
      state_.record_seqs[current.id] = current.seq;
      state_.records[current.id] = std::move(specifics);
      changed.insert(current.id);
    }
    // What ours had that the room no longer has goes here too.
    if (std::optional<Records> local = model_->GetLocal(state_.records)) {
      for (auto& [id, specifics] : gone) {
        if (auto it = local->find(id); it != local->end()) {
          specifics = it->second;
        }
      }
    }
    applying_ = true;
    model_->Apply(state_.records, changed, gone);
    applying_ = false;
    delegate_->OnStateChanged();
    if (++conflicts_ < kMaxConflicts) {
      SendLocalChanges();
    } else {
      conflicts_ = 0;
      RetryLater(base::BindOnce(&RoomSync::SendLocalChanges,
                                weak_factory_.GetWeakPtr()));
    }
    return;
  }
  if (error.is_stale_epoch() || error.is_forbidden()) {
    // A rotation (re-encrypt with the new key) or a new role.
    send_again_ = true;
    FetchRoom();
    return;
  }
  if (error.is_gone()) {
    Fail(error);
    return;
  }
  if (error.is_too_large() || error.is_quota_exceeded()) {
    delegate_->OnWriteRefused(error);
    return;
  }
  RetryLater(base::BindOnce(&RoomSync::SendLocalChanges,
                            weak_factory_.GetWeakPtr()));
}

void RoomSync::Revert(const Records& local) {
  std::set<std::string> changed;
  Records deleted;
  for (const auto& [id, specifics] : state_.records) {
    auto it = local.find(id);
    if (it == local.end() || !SameContent(it->second, specifics)) {
      changed.insert(id);
    }
  }
  for (const auto& [id, specifics] : local) {
    if (!state_.records.contains(id)) {
      deleted[id] = specifics;
    }
  }
  if (changed.empty() && deleted.empty()) {
    return;
  }
  VLOG(1) << "[compass-rooms] view only: undoing " << changed.size() + deleted.size()
          << " changes";
  applying_ = true;
  model_->Apply(state_.records, changed, deleted);
  applying_ = false;
}

void RoomSync::Fail(const RoomError& error) {
  stopped_ = true;
  send_timer_.Stop();
  retry_timer_.Stop();
  delegate_->OnRemoved(/*room_deleted=*/error.status == 404);
}

void RoomSync::RetryLater(base::OnceClosure what) {
  ++failures_;
  const base::TimeDelta delay =
      std::min(kMaxRetryDelay, base::Seconds(1 << std::min(failures_, 8)));
  retry_timer_.Start(FROM_HERE, delay, std::move(what));
}

}  // namespace compass::rooms
