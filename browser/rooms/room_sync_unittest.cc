// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/room_sync.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/uuid.h"
#include "compass/browser/rooms/room_model.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

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

// One room on a pretend server: records by ID, seqs, long polls waiting.
class FakeRoom {
 public:
  struct Poll {
    int64_t since;
    RoomClient::Callback<RoomChanges> callback;
  };

  explicit FakeRoom(uint32_t epoch) : epoch_(epoch) {}

  RoomInfo Info(const std::string& role) const {
    RoomInfo info;
    info.id = "room";
    info.kind = "folder";
    info.epoch = epoch_;
    info.seq = seq_;
    info.role = role;
    return info;
  }

  void GetChanges(int64_t since, RoomClient::Callback<RoomChanges> callback) {
    RoomChanges changes = ChangesSince(since);
    if (changes.records.empty()) {
      polls_.push_back({since, std::move(callback)});
      return;
    }
    std::move(callback).Run(std::move(changes));
  }

  void Commit(const std::string& role,
              const std::vector<RecordWrite>& writes,
              RoomClient::Callback<CommitResult> callback) {
    ++commits_;
    if (role == sync::rooms::kRoleViewer) {
      std::move(callback).Run(base::unexpected(RoomError(403, "forbidden")));
      return;
    }
    RoomError conflict(409, "conflict");
    for (const RecordWrite& write : writes) {
      if (write.epoch != epoch_) {
        std::move(callback).Run(
            base::unexpected(RoomError(409, "stale_epoch")));
        return;
      }
      if (!write.base_seq) {
        continue;
      }
      auto it = records_.find(write.id);
      const int64_t current = it == records_.end() ? 0 : it->second.seq;
      if (current != *write.base_seq) {
        conflict.current.push_back(it == records_.end() ? RoomRecord()
                                                        : it->second);
        conflict.current.back().id = write.id;
      }
    }
    if (!conflict.current.empty()) {
      std::move(callback).Run(base::unexpected(conflict));
      return;
    }
    CommitResult result;
    for (const RecordWrite& write : writes) {
      RoomRecord& record = records_[write.id];
      record.id = write.id;
      record.seq = ++seq_;
      record.epoch = write.epoch;
      record.deleted = write.deleted;
      record.ciphertext = write.deleted ? std::vector<uint8_t>()
                                        : write.ciphertext;
      result.records[write.id] = record.seq;
    }
    result.seq = seq_;
    std::move(callback).Run(std::move(result));
    // Wakes the long polls.
    std::vector<Poll> polls = std::move(polls_);
    for (Poll& poll : polls) {
      GetChanges(poll.since, std::move(poll.callback));
    }
  }

  int commits() const { return commits_; }
  size_t live_records() const {
    size_t n = 0;
    for (const auto& [id, record] : records_) {
      n += !record.deleted;
    }
    return n;
  }

 private:
  RoomChanges ChangesSince(int64_t since) const {
    RoomChanges changes;
    changes.seq = seq_;
    changes.epoch = epoch_;
    std::vector<RoomRecord> records;
    for (const auto& [id, record] : records_) {
      if (record.seq > since) {
        records.push_back(record);
      }
    }
    std::ranges::sort(records, {}, &RoomRecord::seq);
    changes.records = std::move(records);
    return changes;
  }

  uint32_t epoch_;
  int64_t seq_ = 0;
  int commits_ = 0;
  std::map<std::string, RoomRecord> records_;
  std::vector<Poll> polls_;
};

class FakeServer : public RoomServer {
 public:
  FakeServer(FakeRoom* room, std::string role)
      : room_(room), role_(std::move(role)) {}

  void GetRoom(RoomClient::Callback<RoomInfo> callback) override {
    std::move(callback).Run(room_->Info(role_));
  }
  void GetChanges(int64_t since,
                  int wait_seconds,
                  RoomClient::Callback<RoomChanges> callback) override {
    room_->GetChanges(since, std::move(callback));
  }
  void Commit(const std::vector<RecordWrite>& writes,
              RoomClient::Callback<CommitResult> callback) override {
    room_->Commit(role_, writes, std::move(callback));
  }

 private:
  const raw_ptr<FakeRoom> room_;
  const std::string role_;
};

class Delegate : public RoomSync::Delegate {
 public:
  void OnRemoved(bool room_deleted) override { removed = true; }
  bool removed = false;
};

std::u16string Title(const SidebarNode& node) {
  return node.title();
}

class RoomSyncTest : public testing::Test {
 protected:
  RoomSyncTest() : room_(1) {
    room_id_.fill(7);
    key_.fill(9);
    identity_ = sync::rooms::IdentityKeyFromPrivate(sync::rooms::X25519Key{1});
  }

  std::unique_ptr<RoomSync> MakeSync(SpaceList& list,
                                     const std::string& role,
                                     Delegate& delegate) {
    return std::make_unique<RoomSync>(
        room_id_, role, identity_, std::map<uint32_t, RoomKey>{{1u, key_}},
        RoomState(), std::make_unique<FakeServer>(&room_, role),
        CreateFolderRoomModel(&list, nullptr, folder_id_.AsLowercaseString(),
                              base::BindRepeating([] { return Placement(); })),
        &delegate);
  }

  // A's shared folder: "Shared" with two tabs and a subfolder with one.
  void FillOwner() {
    SpaceModel& space = *owner_.spaces()[0];
    folder_ = space.AddFolder(space.pinned_root(), 0, u"Shared", folder_id_);
    space.AddTab(folder_, 0, GURL("https://a.example/"), u"A");
    sub_ = space.AddFolder(folder_, 1, u"Sub");
    space.AddTab(sub_, 0, GURL("https://c.example/"), u"C");
    space.AddTab(folder_, 2, GURL("https://b.example/"), u"B");
  }

  SidebarNode* MemberFolder() {
    return member_.spaces()[0]->FindById(folder_id_);
  }

  void Settle() { task_environment_.FastForwardBy(base::Seconds(5)); }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeRoom room_;
  sync::rooms::RoomId room_id_;
  RoomKey key_;
  sync::rooms::IdentityKey identity_;
  base::Uuid folder_id_ = base::Uuid::GenerateRandomV4();
  SpaceList owner_;
  SpaceList member_;
  raw_ptr<SidebarNode> folder_ = nullptr;
  raw_ptr<SidebarNode> sub_ = nullptr;
  Delegate owner_delegate_;
  Delegate member_delegate_;
};

TEST_F(RoomSyncTest, SharedFolderReachesAMember) {
  FillOwner();
  std::unique_ptr<RoomSync> owner = MakeSync(owner_, "owner", owner_delegate_);
  owner->Start();
  Settle();
  // The folder, three tabs and the subfolder.
  EXPECT_EQ(room_.live_records(), 5u);

  std::unique_ptr<RoomSync> member =
      MakeSync(member_, "editor", member_delegate_);
  member->Start();
  Settle();
  SidebarNode* folder = MemberFolder();
  ASSERT_TRUE(folder);
  EXPECT_EQ(Title(*folder), u"Shared");
  ASSERT_EQ(folder->children().size(), 3u);
  EXPECT_EQ(Title(*folder->children()[0]), u"A");
  EXPECT_EQ(Title(*folder->children()[1]), u"Sub");
  EXPECT_EQ(Title(*folder->children()[2]), u"B");
  ASSERT_EQ(folder->children()[1]->children().size(), 1u);
  EXPECT_EQ(folder->children()[1]->children()[0]->url(),
            GURL("https://c.example/"));
  // Applying wrote nothing back.
  const int commits = room_.commits();
  Settle();
  EXPECT_EQ(room_.commits(), commits);
  // Nothing outside the folder: the member's space has only it.
  EXPECT_EQ(member_.spaces()[0]->pinned_root()->children().size(), 1u);
}

TEST_F(RoomSyncTest, EditsGoBothWays) {
  FillOwner();
  std::unique_ptr<RoomSync> owner = MakeSync(owner_, "owner", owner_delegate_);
  std::unique_ptr<RoomSync> member =
      MakeSync(member_, "editor", member_delegate_);
  owner->Start();
  Settle();
  member->Start();
  Settle();

  // The member renames the folder and adds a tab.
  SpaceModel& space = *member_.spaces()[0];
  SidebarNode* folder = MemberFolder();
  space.SetTitle(folder, u"Renamed");
  space.AddTab(folder, 0, GURL("https://new.example/"), u"New");
  member->NotifyLocalChange();
  Settle();
  EXPECT_EQ(Title(*folder_), u"Renamed");
  ASSERT_EQ(folder_->children().size(), 4u);
  EXPECT_EQ(folder_->children()[0]->url(), GURL("https://new.example/"));

  // The owner deletes the subfolder and moves B first.
  SpaceModel& owner_space = *owner_.spaces()[0];
  owner_space.Remove(sub_);
  owner_space.Move(folder_->children().back().get(), folder_, 0);
  owner->NotifyLocalChange();
  Settle();
  ASSERT_EQ(folder->children().size(), 3u);
  EXPECT_EQ(Title(*folder->children()[0]), u"B");
  EXPECT_EQ(Title(*folder->children()[1]), u"New");
}

TEST_F(RoomSyncTest, ViewerEditsAreUndone) {
  FillOwner();
  std::unique_ptr<RoomSync> owner = MakeSync(owner_, "owner", owner_delegate_);
  owner->Start();
  Settle();
  std::unique_ptr<RoomSync> viewer =
      MakeSync(member_, "viewer", member_delegate_);
  viewer->Start();
  Settle();
  ASSERT_TRUE(viewer->read_only());
  const int commits = room_.commits();

  SpaceModel& space = *member_.spaces()[0];
  SidebarNode* folder = MemberFolder();
  space.SetTitle(folder, u"Mine now");
  space.AddTab(folder, 0, GURL("https://x.example/"), u"X");
  space.Remove(folder->children().back().get());
  viewer->NotifyLocalChange();
  Settle();
  EXPECT_EQ(room_.commits(), commits);
  EXPECT_EQ(Title(*folder), u"Shared");
  ASSERT_EQ(folder->children().size(), 3u);
  EXPECT_EQ(Title(*folder->children()[0]), u"A");
  EXPECT_EQ(Title(*folder->children()[2]), u"B");
}

TEST_F(RoomSyncTest, ConflictKeepsTheLaterWrite) {
  FillOwner();
  std::unique_ptr<RoomSync> owner = MakeSync(owner_, "owner", owner_delegate_);
  std::unique_ptr<RoomSync> member =
      MakeSync(member_, "editor", member_delegate_);
  owner->Start();
  Settle();
  member->Start();
  Settle();
  // Both rename the folder before seeing the other's change: the second
  // write meets a conflict, merges and wins.
  owner_.spaces()[0]->SetTitle(folder_, u"Owner's");
  member_.spaces()[0]->SetTitle(MemberFolder(), u"Member's");
  owner->SendLocalChangesForTesting();
  member->SendLocalChangesForTesting();
  Settle();
  EXPECT_EQ(Title(*folder_), Title(*MemberFolder()));
}

TEST_F(RoomSyncTest, StateRoundTrips) {
  RoomState state;
  state.seq = 42;
  state.records["a"].mutable_node()->set_title("A");
  state.record_seqs["a"] = 40;
  state.record_seqs["gone"] = 41;
  const RoomState back = RoomState::FromValue(state.ToValue());
  EXPECT_EQ(back.seq, 42);
  ASSERT_TRUE(back.records.contains("a"));
  EXPECT_EQ(back.records.at("a").node().title(), "A");
  EXPECT_EQ(back.record_seqs.at("gone"), 41);
  EXPECT_FALSE(back.records.contains("gone"));
}

// A folder shared on its own inside a shared folder belongs to its own room:
// the outer room neither writes it nor applies it.
TEST_F(RoomSyncTest, NestedRoomsAreLeftOut) {
  FillOwner();
  std::unique_ptr<RoomModel> model = CreateFolderRoomModel(
      &owner_, nullptr, folder_id_.AsLowercaseString(),
      base::BindRepeating([] { return Placement(); }));
  const std::string sub = sub_->id().AsLowercaseString();
  const std::string in_sub = sub_->children()[0]->id().AsLowercaseString();
  std::optional<Records> all = model->GetLocal({});
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->contains(sub));
  EXPECT_TRUE(all->contains(in_sub));
  EXPECT_TRUE(model->Contains(*sub_->children()[0]));

  model->set_excluded({sub});
  std::optional<Records> outer = model->GetLocal({});
  ASSERT_TRUE(outer);
  EXPECT_FALSE(outer->contains(sub));
  EXPECT_FALSE(outer->contains(in_sub));
  EXPECT_EQ(outer->size(), 3u);
  EXPECT_FALSE(model->Contains(*sub_->children()[0]));

  // A record of it in the outer room (an older client's) doesn't move it.
  Records mirror = *all;
  mirror[sub].mutable_node()->set_title("Moved");
  model->Apply(mirror, {sub}, {});
  EXPECT_EQ(sub_->title(), u"Sub");
}

}  // namespace
}  // namespace compass::rooms
