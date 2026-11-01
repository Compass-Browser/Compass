// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Slice 5 at the bridge level (docs/design/sync.md, "Slice 5 notes"): the
// chain's profile list in the home profile's account, spaces moving between
// two profiles' accounts, and directory positions comparable across
// accounts. Each device has two profiles, each with its own list and
// bridge; each profile has its own account (an in-memory server).

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/sync/compass_spaces_sync_bridge.h"
#include "components/sync/base/client_tag_hash.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/deletion_origin.h"
#include "components/sync/model/data_type_store.h"
#include "components/sync/model/entity_change.h"
#include "components/sync/model/metadata_change_list.h"
#include "components/sync/protocol/entity_data.h"
#include "components/sync/protocol/entity_specifics.pb.h"
#include "components/sync/test/data_type_store_test_util.h"
#include "components/sync/test/mock_data_type_local_change_processor.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

using ::testing::_;
using ::testing::NiceMock;

constexpr char kPersonal[] = "00000000-0000-4000-8000-0000000000a1";
constexpr char kWork[] = "00000000-0000-4000-8000-0000000000b2";

std::string Key(const base::Uuid& id) {
  return id.AsLowercaseString();
}

std::string KeyOf(const sync_pb::CompassSpacesSpecifics& specifics) {
  if (specifics.has_space()) {
    return specifics.space().uuid();
  }
  if (specifics.has_profile()) {
    return specifics.profile().uuid();
  }
  return specifics.node().uuid();
}

// A profile's account on the server.
struct Account {
  struct Change {
    int from;
    std::string key;
    std::optional<sync_pb::CompassSpacesSpecifics> specifics;
  };
  std::map<std::string, sync_pb::CompassSpacesSpecifics> entities;
  std::vector<Change> log;

  size_t CountProfiles(bool home) const {
    return static_cast<size_t>(
        std::ranges::count_if(entities, [home](const auto& entry) {
          return entry.second.has_profile() &&
                 entry.second.profile().home() == home;
        }));
  }
};

class Device;

// One profile of a device: its spaces, and a bridge syncing them with its
// account. Its delegate stands in for compass_spaces_sync.cc and
// CompassSyncService.
class ProfileSide : public CompassSpacesSyncBridge::Delegate {
 public:
  ProfileSide(Device* device,
              int id,
              Account* account,
              std::string uuid,
              base::test::TaskEnvironment* task_environment)
      : device_(device),
        id_(id),
        account_(account),
        uuid_(std::move(uuid)),
        task_environment_(task_environment) {
    ON_CALL(processor_, IsTrackingMetadata()).WillByDefault([this] {
      return tracking_;
    });
    ON_CALL(processor_, Put(_, _, _))
        .WillByDefault([this](const std::string& key,
                              std::unique_ptr<syncer::EntityData> data,
                              syncer::MetadataChangeList*) {
          puts_[key] = data->specifics.compass_spaces();
          deletes_.erase(key);
        });
    ON_CALL(processor_, Delete(_, _, _))
        .WillByDefault([this](const std::string& key,
                              const syncer::DeletionOrigin&,
                              syncer::MetadataChangeList*) {
          puts_.erase(key);
          deletes_.insert(key);
        });
    store_ = syncer::DataTypeStoreTestUtil::CreateInMemoryStoreForTest(
        syncer::COMPASS_SPACES);
    bridge_ = std::make_unique<CompassSpacesSyncBridge>(
        processor_.CreateForwardingProcessor(),
        syncer::DataTypeStoreTestUtil::FactoryForForwardingStore(store_.get()),
        &list_, this);
    bridge_->OnModelLoaded();
    task_environment_->RunUntilIdle();
  }

  // The initial merge with the account, then its commits.
  void Join() {
    Merge();
    Push();
  }

  // The initial merge alone: its commits wait for Push().
  void Merge() {
    syncer::EntityChangeList changes;
    for (const auto& [key, specifics] : account_->entities) {
      changes.push_back(syncer::EntityChange::CreateAdd(key, Data(specifics)));
    }
    cursor_ = account_->log.size();
    tracking_ = true;
    EXPECT_FALSE(bridge_->MergeFullSyncData(bridge_->CreateMetadataChangeList(),
                                            std::move(changes)));
    task_environment_->RunUntilIdle();
  }

  // Commits local changes. Returns how many.
  size_t Push() {
    bridge_->SendLocalChangesForTesting();
    task_environment_->RunUntilIdle();
    const size_t count = puts_.size() + deletes_.size();
    for (const auto& [key, specifics] : puts_) {
      account_->entities[key] = specifics;
      account_->log.push_back({id_, key, specifics});
    }
    for (const std::string& key : deletes_) {
      account_->entities.erase(key);
      account_->log.push_back({id_, key, std::nullopt});
    }
    puts_.clear();
    deletes_.clear();
    return count;
  }

  // Applies the other devices' changes since the last pull.
  void Pull() {
    std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
        latest;
    std::vector<std::string> order;
    for (size_t i = cursor_; i < account_->log.size(); ++i) {
      const Account::Change& change = account_->log[i];
      if (change.from == id_) {
        continue;
      }
      if (!latest.contains(change.key)) {
        order.push_back(change.key);
      }
      latest[change.key] = change.specifics;
    }
    cursor_ = account_->log.size();
    syncer::EntityChangeList changes;
    for (const std::string& key : order) {
      if (latest[key]) {
        changes.push_back(
            syncer::EntityChange::CreateUpdate(key, Data(*latest[key])));
      } else {
        changes.push_back(
            syncer::EntityChange::CreateDelete(key, syncer::EntityData()));
      }
    }
    EXPECT_FALSE(bridge_->ApplyIncrementalSyncChanges(
        bridge_->CreateMetadataChangeList(), std::move(changes)));
    task_environment_->RunUntilIdle();
  }

  static syncer::EntityData Data(
      const sync_pb::CompassSpacesSpecifics& specifics) {
    syncer::EntityData data;
    *data.specifics.mutable_compass_spaces() = specifics;
    data.client_tag_hash = syncer::ClientTagHash::FromUnhashed(
        syncer::COMPASS_SPACES, KeyOf(specifics));
    return data;
  }

  // The home profile's list of the chain's profiles here.
  void SetLocalProfiles(std::vector<sync_pb::CompassProfile> profiles) {
    local_profiles_ = std::move(profiles);
    bridge_->NotifyLocalChange();
  }

  // CompassSpacesSyncBridge::Delegate:
  void DeleteSpace(SpaceModel& space) override {
    if (list_.spaces().size() > 1) {
      list_.RemoveSpace(space);
    }
  }
  void MoveEntry(SidebarNode* node,
                 SpaceModel& target,
                 SidebarNode* parent,
                 size_t index) override {
    SpaceModel* source = list_.favorites().Contains(node)
                             ? &list_.favorites()
                             : list_.FindSpaceOf(node);
    source->MoveToSpace(node, target, parent, index);
  }
  std::optional<std::vector<sync_pb::CompassProfile>> GetLocalProfiles()
      override {
    return local_profiles_;
  }
  void OnChainProfiles(
      const std::vector<sync_pb::CompassProfile>& profiles) override;
  std::string GetProfileUuid() override { return uuid_; }
  SpaceModel* AdoptSpace(const base::Uuid& id) override;
  std::vector<DirectorySlot> GetDirectoryOrder() override;
  void OnSpacePositions(
      const std::map<std::string, syncer::UniquePosition>& positions,
      bool remote) override;

  SpaceList& list() { return list_; }
  CompassSpacesSyncBridge& bridge() { return *bridge_; }
  const std::string& uuid() const { return uuid_; }
  const std::vector<sync_pb::CompassProfile>& chain_profiles() const {
    return chain_profiles_;
  }

 private:
  const raw_ptr<Device> device_;
  const int id_;
  const raw_ptr<Account> account_;
  const std::string uuid_;
  const raw_ptr<base::test::TaskEnvironment> task_environment_;
  SpaceList list_;
  NiceMock<syncer::MockDataTypeLocalChangeProcessor> processor_;
  std::unique_ptr<syncer::DataTypeStore> store_;
  std::unique_ptr<CompassSpacesSyncBridge> bridge_;
  bool tracking_ = false;
  std::map<std::string, sync_pb::CompassSpacesSpecifics> puts_;
  std::set<std::string> deletes_;
  size_t cursor_ = 0;
  std::optional<std::vector<sync_pb::CompassProfile>> local_profiles_;
  std::vector<sync_pb::CompassProfile> chain_profiles_;
};

// A device with two profiles, Personal (the home) and Work, and the
// switcher's order across them.
class Device {
 public:
  Device(int id,
         Account* personal,
         Account* work,
         base::test::TaskEnvironment* task_environment) {
    personal_ = std::make_unique<ProfileSide>(this, id, personal, kPersonal,
                                              task_environment);
    work_ = std::make_unique<ProfileSide>(this, id, work, kWork,
                                          task_environment);
  }

  ProfileSide& personal() { return *personal_; }
  ProfileSide& work() { return *work_; }

  // The switcher: |order_|, then spaces it doesn't have yet.
  std::vector<std::pair<ProfileSide*, SpaceModel*>> Directory() {
    std::vector<std::pair<ProfileSide*, SpaceModel*>> all;
    for (ProfileSide* side : {personal_.get(), work_.get()}) {
      for (const auto& space : side->list().spaces()) {
        all.emplace_back(side, space.get());
      }
    }
    auto rank = [this](const std::pair<ProfileSide*, SpaceModel*>& entry) {
      return std::ranges::find(order_, entry.second->id()) - order_.begin();
    };
    std::ranges::stable_sort(all, {}, rank);
    return all;
  }
  void SetOrder(std::vector<base::Uuid> order) { order_ = std::move(order); }
  std::vector<std::string> DirectoryNames() {
    std::vector<std::string> names;
    for (const auto& [side, space] : Directory()) {
      names.push_back(base::UTF16ToUTF8(space->name()));
    }
    return names;
  }

  std::vector<DirectorySlot> DirectoryOrder(ProfileSide* side) {
    std::vector<DirectorySlot> order;
    for (const auto& [owner, space] : Directory()) {
      const std::string key = Key(space->id());
      std::optional<syncer::UniquePosition> position;
      auto it = positions_.find(key);
      if (owner != side && it != positions_.end()) {
        position = it->second;
      }
      order.emplace_back(key, std::move(position));
    }
    return order;
  }

  void OnPositions(
      const std::map<std::string, syncer::UniquePosition>& positions,
      bool remote) {
    for (const auto& [key, position] : positions) {
      positions_.insert_or_assign(key, position);
    }
    if (!remote) {
      return;
    }
    // As SortDirectoryByPositions(): spaces with a position sort by it in
    // their slots.
    std::vector<std::pair<ProfileSide*, SpaceModel*>> all = Directory();
    std::vector<size_t> slots;
    std::vector<std::pair<syncer::UniquePosition, SpaceModel*>> positioned;
    for (size_t i = 0; i < all.size(); ++i) {
      auto it = positions_.find(Key(all[i].second->id()));
      if (it != positions_.end()) {
        slots.push_back(i);
        positioned.emplace_back(it->second, all[i].second);
      }
    }
    std::ranges::stable_sort(positioned, [](const auto& a, const auto& b) {
      return a.first.LessThan(b.first);
    });
    std::vector<base::Uuid> order;
    for (const auto& [side, space] : all) {
      order.push_back(space->id());
    }
    for (size_t k = 0; k < slots.size(); ++k) {
      order[slots[k]] = positioned[k].second->id();
    }
    order_ = std::move(order);
  }

  // MoveSpaceToProfile() at the model level: the same space (ID, entries)
  // leaves |from| for |to|, in the same place.
  SpaceModel* MoveSpace(ProfileSide& from, SpaceModel& space, ProfileSide& to) {
    SpaceModel* moved = to.list().AddSpace(to.list().spaces().size(),
                                           space.name(), space.id());
    moved->SetColor(space.color());
    for (auto [from_root, to_root] :
         {std::pair(space.pinned_root(), moved->pinned_root()),
          std::pair(space.today_root(), moved->today_root())}) {
      while (!from_root->children().empty()) {
        space.MoveToSpace(from_root->children().front().get(), *moved,
                          to_root, to_root->children().size());
      }
    }
    from.list().RemoveSpace(space);
    return moved;
  }

  SpaceModel* Adopt(const base::Uuid& id, ProfileSide* to) {
    for (ProfileSide* side : {personal_.get(), work_.get()}) {
      if (side == to) {
        continue;
      }
      if (SpaceModel* space = side->list().FindSpace(id)) {
        return MoveSpace(*side, *space, *to);
      }
    }
    return nullptr;
  }

 private:
  std::unique_ptr<ProfileSide> personal_;
  std::unique_ptr<ProfileSide> work_;
  std::vector<base::Uuid> order_;
  std::map<std::string, syncer::UniquePosition> positions_;
};

void ProfileSide::OnChainProfiles(
    const std::vector<sync_pb::CompassProfile>& profiles) {
  chain_profiles_ = profiles;
  if (!local_profiles_) {
    return;
  }
  // As CompassSyncService: a home that hasn't published its UUID takes the
  // chain's; of two published ones, the smallest wins. Names and avatars
  // follow the chain.
  for (sync_pb::CompassProfile& local : *local_profiles_) {
    if (!local.home()) {
      continue;
    }
    const std::string own = local.uuid();
    const bool published = std::ranges::any_of(
        profiles, [&own](const sync_pb::CompassProfile& p) {
          return p.home() && p.uuid() == own;
        });
    for (const sync_pb::CompassProfile& chain : profiles) {
      if (chain.home() && (local.uuid() == own
                               ? !published || chain.uuid() < own
                               : chain.uuid() < local.uuid())) {
        local.set_uuid(chain.uuid());
      }
    }
  }
  for (sync_pb::CompassProfile& local : *local_profiles_) {
    for (const sync_pb::CompassProfile& chain : profiles) {
      if (chain.uuid() == local.uuid()) {
        local.set_name(chain.name());
        if (chain.avatar() > 0) {
          local.set_avatar(chain.avatar());
        }
      }
    }
  }
}

SpaceModel* ProfileSide::AdoptSpace(const base::Uuid& id) {
  return device_->Adopt(id, this);
}

std::vector<DirectorySlot> ProfileSide::GetDirectoryOrder() {
  return device_->DirectoryOrder(this);
}

void ProfileSide::OnSpacePositions(
    const std::map<std::string, syncer::UniquePosition>& positions,
    bool remote) {
  device_->OnPositions(positions, remote);
}

sync_pb::CompassProfile MakeProfile(const std::string& uuid,
                                    const std::string& name,
                                    int avatar,
                                    bool home) {
  sync_pb::CompassProfile profile;
  profile.set_uuid(uuid);
  profile.set_name(name);
  if (avatar > 0) {
    profile.set_avatar(avatar);
  }
  if (home) {
    profile.set_home(true);
  }
  return profile;
}

class CompassSpacesSyncProfilesTest : public testing::Test {
 protected:
  std::unique_ptr<Device> MakeDevice() {
    return std::make_unique<Device>(next_id_++, &personal_, &work_,
                                    &task_environment_);
  }

  base::test::TaskEnvironment task_environment_;
  Account personal_;
  Account work_;
  int next_id_ = 1;
};

TEST_F(CompassSpacesSyncProfilesTest, TheHomeAccountCarriesTheProfileList) {
  std::unique_ptr<Device> a = MakeDevice();
  a->personal().SetLocalProfiles(
      {MakeProfile(kPersonal, "Personal", 27, true),
       MakeProfile(kWork, "Work", 0, false)});
  a->personal().Join();
  a->work().Join();

  // In the home account only: the home and Work.
  ASSERT_TRUE(personal_.entities.contains(kPersonal));
  const sync_pb::CompassProfile& home =
      personal_.entities[kPersonal].profile();
  EXPECT_EQ(home.name(), "Personal");
  EXPECT_EQ(home.avatar(), 27);
  EXPECT_TRUE(home.home());
  ASSERT_TRUE(personal_.entities.contains(kWork));
  EXPECT_EQ(personal_.entities[kWork].profile().name(), "Work");
  EXPECT_FALSE(personal_.entities[kWork].profile().has_avatar());
  EXPECT_EQ(work_.CountProfiles(true) + work_.CountProfiles(false), 0u);
  // Spaces carry their profile's UUID.
  const std::string space = Key(a->work().list().spaces()[0]->id());
  EXPECT_EQ(work_.entities[space].space().profile_uuid(), kWork);

  // B's home learns the list at its first sync, and takes the chain's names:
  // nothing echoes.
  std::unique_ptr<Device> b = MakeDevice();
  b->personal().SetLocalProfiles(
      {MakeProfile(kPersonal, "Person 1", 0, true)});
  b->personal().Join();
  ASSERT_EQ(b->personal().chain_profiles().size(), 2u);
  EXPECT_EQ(b->personal().Push(), 0u);
  EXPECT_EQ(personal_.entities[kPersonal].profile().name(), "Personal");

  // A renames Work: one commit, which B applies without sending it back.
  a->personal().SetLocalProfiles(
      {MakeProfile(kPersonal, "Personal", 27, true),
       MakeProfile(kWork, "Job", 0, false)});
  EXPECT_EQ(a->personal().Push(), 1u);
  b->personal().Pull();
  EXPECT_EQ(b->personal().Push(), 0u);
  auto job = std::ranges::find_if(
      b->personal().chain_profiles(),
      [](const sync_pb::CompassProfile& p) { return p.uuid() == kWork; });
  ASSERT_NE(job, b->personal().chain_profiles().end());
  EXPECT_EQ(job->name(), "Job");

  // A device's missing profile isn't deleted from the chain: B doesn't have
  // Work, and Work stays.
  EXPECT_TRUE(personal_.entities.contains(kWork));
}

TEST_F(CompassSpacesSyncProfilesTest, AJoiningHomeTakesTheChainsUuid) {
  std::unique_ptr<Device> a = MakeDevice();
  const std::string a_home = "00000000-0000-4000-8000-0000000000f0";
  const std::string b_home = "00000000-0000-4000-8000-000000000001";
  a->personal().SetLocalProfiles({MakeProfile(a_home, "Personal", 0, true)});
  a->personal().Join();

  // B's own UUID is smaller, but it never published it: it takes A's.
  std::unique_ptr<Device> b = MakeDevice();
  b->personal().SetLocalProfiles({MakeProfile(b_home, "Personal", 0, true)});
  b->personal().Join();
  EXPECT_EQ(b->personal().GetLocalProfiles()->front().uuid(), a_home);
  EXPECT_EQ(personal_.CountProfiles(/*home=*/true), 1u);
  EXPECT_TRUE(personal_.entities.contains(a_home));
}

TEST_F(CompassSpacesSyncProfilesTest, TwoPublishedHomesBecomeOne) {
  const std::string a_home = "00000000-0000-4000-8000-0000000000f0";
  const std::string b_home = "00000000-0000-4000-8000-000000000001";
  std::unique_ptr<Device> a = MakeDevice();
  std::unique_ptr<Device> b = MakeDevice();
  a->personal().SetLocalProfiles({MakeProfile(a_home, "Personal", 0, true)});
  b->personal().SetLocalProfiles({MakeProfile(b_home, "Personal", 0, true)});
  // Both merge with an empty account before either commits.
  a->personal().Merge();
  b->personal().Merge();
  a->personal().Push();
  b->personal().Push();
  ASSERT_EQ(personal_.CountProfiles(/*home=*/true), 2u);

  // A sees B's smaller one: it takes it and deletes its own.
  a->personal().Pull();
  EXPECT_EQ(a->personal().GetLocalProfiles()->front().uuid(), b_home);
  EXPECT_EQ(a->personal().Push(), 1u);
  EXPECT_EQ(personal_.CountProfiles(/*home=*/true), 1u);
  EXPECT_TRUE(personal_.entities.contains(b_home));

  // B keeps its own: nothing more to send.
  b->personal().Pull();
  EXPECT_EQ(b->personal().Push(), 0u);
  EXPECT_EQ(b->personal().GetLocalProfiles()->front().uuid(), b_home);
  EXPECT_EQ(personal_.CountProfiles(/*home=*/true), 1u);
}

// A and B both have Personal and Work. A moves Work's "Projects" space to
// Personal: Work's account deletes it, Personal's creates it under the same
// IDs. B gets both changes, in either order.
class CompassSpacesSyncMoveTest
    : public CompassSpacesSyncProfilesTest,
      public testing::WithParamInterface<bool /*personal_first*/> {};

TEST_P(CompassSpacesSyncMoveTest, ASpaceFollowsItsProfile) {
  std::unique_ptr<Device> a = MakeDevice();
  SpaceModel* projects =
      a->work().list().AddSpace(1, u"Projects", std::nullopt);
  projects->AddTab(projects->pinned_root(), 0, GURL("https://plan.test/"),
                   u"Plan");
  SidebarNode* folder =
      projects->AddFolder(projects->pinned_root(), 1, u"Specs");
  projects->AddTab(folder, 0, GURL("https://spec.test/"), u"Spec");
  const base::Uuid id = projects->id();
  const std::string key = Key(id);
  a->personal().Join();
  a->work().Join();
  ASSERT_TRUE(work_.entities.contains(key));

  std::unique_ptr<Device> b = MakeDevice();
  b->personal().Join();
  b->work().Join();
  SpaceModel* on_b = b->work().list().FindSpace(id);
  ASSERT_TRUE(on_b);
  EXPECT_EQ(on_b->pinned_root()->children().size(), 2u);

  // A moves it.
  a->MoveSpace(a->work(), *projects, a->personal());
  EXPECT_EQ(a->work().Push(), 4u);      // the space, two tabs, the folder
  EXPECT_EQ(a->personal().Push(), 4u);  // the same, created
  EXPECT_FALSE(work_.entities.contains(key));
  ASSERT_TRUE(personal_.entities.contains(key));
  EXPECT_EQ(personal_.entities[key].space().profile_uuid(), kPersonal);

  if (GetParam()) {
    // Personal's change first: B moves its copy over, entries and all.
    b->personal().Pull();
    EXPECT_FALSE(b->work().list().FindSpace(id));
    b->work().Pull();
  } else {
    // Work's first: the space goes, then comes back in Personal.
    b->work().Pull();
    EXPECT_FALSE(b->work().list().FindSpace(id));
    b->personal().Pull();
  }
  SpaceModel* moved = b->personal().list().FindSpace(id);
  ASSERT_TRUE(moved);
  EXPECT_EQ(moved->name(), u"Projects");
  ASSERT_EQ(moved->pinned_root()->children().size(), 2u);
  EXPECT_EQ(moved->pinned_root()->children()[0]->title(), u"Plan");
  EXPECT_EQ(moved->pinned_root()->children()[1]->children().size(), 1u);
  EXPECT_FALSE(b->work().list().FindSpace(id));

  // Nothing new to send from B: Personal has what A sent; Work's deletions
  // are the ones A made (or nothing, if they came first).
  EXPECT_EQ(b->personal().Push(), 0u);
  b->work().Push();
  EXPECT_FALSE(work_.entities.contains(key));
  EXPECT_TRUE(personal_.entities.contains(key));
  a->personal().Pull();
  a->work().Pull();
  EXPECT_EQ(a->personal().Push(), 0u);
  EXPECT_EQ(a->work().Push(), 0u);
}

INSTANTIATE_TEST_SUITE_P(All, CompassSpacesSyncMoveTest, testing::Bool());

TEST_F(CompassSpacesSyncProfilesTest, DirectoryPositionsInterleaveAccounts) {
  std::unique_ptr<Device> a = MakeDevice();
  SpaceModel* p1 = a->personal().list().spaces()[0].get();
  p1->SetName(u"P1");
  SpaceModel* p2 = a->personal().list().AddSpace(1, u"P2", std::nullopt);
  SpaceModel* w1 = a->work().list().spaces()[0].get();
  w1->SetName(u"W1");
  a->SetOrder({p1->id(), w1->id(), p2->id()});
  a->personal().Join();
  a->work().Join();

  auto position = [](Account& account, const SpaceModel* space) {
    return syncer::UniquePosition::FromProto(
        account.entities[Key(space->id())].space().directory_position());
  };
  const syncer::UniquePosition sp1 = position(personal_, p1);
  const syncer::UniquePosition sw1 = position(work_, w1);
  const syncer::UniquePosition sp2 = position(personal_, p2);
  ASSERT_TRUE(sp1.IsValid() && sw1.IsValid() && sp2.IsValid());
  EXPECT_TRUE(sp1.LessThan(sw1));
  EXPECT_TRUE(sw1.LessThan(sp2));

  // B, with its untouched starting spaces, gets the same order from two
  // accounts.
  std::unique_ptr<Device> b = MakeDevice();
  b->personal().Join();
  b->work().Join();
  EXPECT_THAT(b->DirectoryNames(), testing::ElementsAre("P1", "W1", "P2"));

  // A moves W1 to the end: only W1's position changes, in Work's account.
  a->SetOrder({p1->id(), p2->id(), w1->id()});
  a->work().bridge().NotifyLocalChange();
  a->personal().bridge().NotifyLocalChange();
  EXPECT_EQ(a->personal().Push(), 0u);
  EXPECT_EQ(a->work().Push(), 1u);
  EXPECT_TRUE(position(personal_, p2).LessThan(position(work_, w1)));
  b->work().Pull();
  EXPECT_THAT(b->DirectoryNames(), testing::ElementsAre("P1", "P2", "W1"));
}

TEST_F(CompassSpacesSyncProfilesTest, ProfileEntitiesAreValid) {
  std::unique_ptr<Device> a = MakeDevice();
  syncer::EntityData data;
  data.specifics.mutable_compass_spaces()->mutable_profile()->set_uuid("bad");
  EXPECT_FALSE(a->personal().bridge().IsEntityDataValid(data));
  data.specifics.mutable_compass_spaces()->mutable_profile()->set_uuid(kWork);
  EXPECT_TRUE(a->personal().bridge().IsEntityDataValid(data));
  EXPECT_EQ(a->personal().bridge().GetStorageKey(data), kWork);
}

}  // namespace
}  // namespace compass::sync
