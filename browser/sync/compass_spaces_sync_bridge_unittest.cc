// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_spaces_sync_bridge.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
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

using Type = SidebarNode::Type;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

std::string Key(const base::Uuid& id) {
  return id.AsLowercaseString();
}

// |parent|'s subtree as text: tabs by title, "Folder{...}" ("Folder-{...}"
// if collapsed), "[pane|pane]".
std::string Describe(const SidebarNode& parent) {
  std::string result;
  for (const auto& child : parent.children()) {
    if (!result.empty()) {
      result += ",";
    }
    const std::string title = base::UTF16ToUTF8(child->display_title());
    switch (child->type()) {
      case Type::kTab:
        result += title;
        break;
      case Type::kFolder:
        result += title + (child->collapsed() ? "-" : "") + "{" +
                  Describe(*child) + "}";
        break;
      case Type::kSplit: {
        std::string panes = Describe(*child);
        std::replace(panes.begin(), panes.end(), ',', '|');
        result += "[" + panes + "]";
        break;
      }
      case Type::kRoot:
        break;
    }
  }
  return result;
}

// Spaces (name, color, pinned tree) in order, then the favorites.
std::string Describe(const SpaceList& list) {
  std::string result;
  for (const auto& space : list.spaces()) {
    result += base::UTF16ToUTF8(space->name()) + "/" +
              base::NumberToString(static_cast<int>(space->color())) + "(" +
              Describe(*space->pinned_root()) + ") ";
  }
  return result + "fav(" + Describe(*list.favorites().pinned_root()) + ")";
}

SidebarNode* FindByTitle(const SidebarNode& parent, std::u16string title) {
  for (const auto& child : parent.children()) {
    if (child->display_title() == title) {
      return child.get();
    }
    if (SidebarNode* found = FindByTitle(*child, title)) {
      return found;
    }
  }
  return nullptr;
}

// A server: the entities, and a log of changes each device reads from.
struct Server {
  struct Change {
    int from;
    std::string key;
    std::optional<sync_pb::CompassSpacesSpecifics> specifics;
  };
  std::map<std::string, sync_pb::CompassSpacesSpecifics> entities;
  std::vector<Change> log;
};

// One Compass: its spaces, a processor that records commits, a store, and
// the bridge.
class Device {
 public:
  Device(int id, Server* server) : id_(id), server_(server) {
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
  }

  // Starts (or restarts) the bridge on the same store.
  void StartBridge() {
    bridge_.reset();
    bridge_ = std::make_unique<CompassSpacesSyncBridge>(
        processor_.CreateForwardingProcessor(),
        syncer::DataTypeStoreTestUtil::FactoryForForwardingStore(store_.get()),
        &list_, /*delegate=*/nullptr);
    bridge_->OnModelLoaded();
    task_environment_->RunUntilIdle();
  }

  // The initial merge with what the server has.
  void Join() {
    syncer::EntityChangeList changes;
    for (const auto& [key, specifics] : server_->entities) {
      changes.push_back(syncer::EntityChange::CreateAdd(key, Data(specifics)));
    }
    cursor_ = server_->log.size();
    tracking_ = true;
    EXPECT_FALSE(bridge_->MergeFullSyncData(bridge_->CreateMetadataChangeList(),
                                            std::move(changes)));
    task_environment_->RunUntilIdle();
    Push();
  }

  // Commits local changes to the server. Returns how many.
  size_t Push() {
    bridge_->SendLocalChangesForTesting();
    task_environment_->RunUntilIdle();
    const size_t count = puts_.size() + deletes_.size();
    for (const auto& [key, specifics] : puts_) {
      server_->entities[key] = specifics;
      server_->log.push_back({id_, key, specifics});
    }
    for (const std::string& key : deletes_) {
      server_->entities.erase(key);
      server_->log.push_back({id_, key, std::nullopt});
    }
    last_push_ = count;
    puts_.clear();
    deletes_.clear();
    return count;
  }

  // Applies the other devices' changes since the last pull, then commits
  // what that made it change (fix-ups; last_push() says how many).
  void Pull() {
    std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
        latest;
    std::vector<std::string> order;
    for (size_t i = cursor_; i < server_->log.size(); ++i) {
      const Server::Change& change = server_->log[i];
      if (change.from == id_) {
        continue;
      }
      if (!latest.contains(change.key)) {
        order.push_back(change.key);
      }
      latest[change.key] = change.specifics;
    }
    cursor_ = server_->log.size();
    ApplyRemote(order, latest);
    Push();
  }

  void ApplyRemote(
      const std::vector<std::string>& order,
      std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>&
          latest) {
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
        syncer::COMPASS_SPACES, specifics.has_space()
                                    ? specifics.space().uuid()
                                    : specifics.node().uuid());
    return data;
  }

  SpaceList& list() { return list_; }
  SpaceModel& space(size_t i) { return *list_.spaces()[i]; }
  SpaceModel& favorites() { return list_.favorites(); }
  CompassSpacesSyncBridge& bridge() { return *bridge_; }
  NiceMock<syncer::MockDataTypeLocalChangeProcessor>& processor() {
    return processor_;
  }
  const std::map<std::string, sync_pb::CompassSpacesSpecifics>& puts() const {
    return puts_;
  }
  size_t last_push() const { return last_push_; }
  void set_task_environment(base::test::TaskEnvironment* environment) {
    task_environment_ = environment;
  }

 private:
  const int id_;
  const raw_ptr<Server> server_;
  raw_ptr<base::test::TaskEnvironment> task_environment_ = nullptr;
  SpaceList list_;
  NiceMock<syncer::MockDataTypeLocalChangeProcessor> processor_;
  std::unique_ptr<syncer::DataTypeStore> store_;
  std::unique_ptr<CompassSpacesSyncBridge> bridge_;
  bool tracking_ = false;
  std::map<std::string, sync_pb::CompassSpacesSpecifics> puts_;
  std::set<std::string> deletes_;
  size_t cursor_ = 0;
  size_t last_push_ = 0;
};

class CompassSpacesSyncBridgeTest : public testing::Test {
 protected:
  std::unique_ptr<Device> MakeDevice() {
    auto device = std::make_unique<Device>(next_id_++, &server_);
    device->set_task_environment(&task_environment_);
    device->StartBridge();
    return device;
  }

  // A's pinned "Work" folder with a tab, a tab, a favorite; space renamed
  // and colored.
  void Populate(Device& device) {
    SpaceModel& space = device.space(0);
    space.SetName(u"Main");
    space.SetColor(SpaceColor::kBlue);
    SidebarNode* pinned = space.pinned_root();
    SidebarNode* work = space.AddFolder(pinned, 0, u"Work");
    space.AddTab(work, 0, GURL("https://mail.test/"), u"Mail");
    space.AddTab(pinned, 1, GURL("https://news.test/"), u"News");
    space.AddTab(space.today_root(), 0, GURL("https://today.test/"),
                 u"Today tab");
    device.favorites().AddTab(device.favorites().pinned_root(), 0,
                              GURL("https://fav.test/"), u"Fav");
  }

  base::test::TaskEnvironment task_environment_;
  Server server_;
  int next_id_ = 1;
};

TEST(CompassSpacesAssignPositionsTest, KeepsTheLongestRunInOrder) {
  const syncer::UniquePosition::Suffix suffix =
      syncer::UniquePosition::RandomSuffix();
  const syncer::UniquePosition p1 =
      syncer::UniquePosition::InitialPosition(suffix);
  const syncer::UniquePosition p2 = syncer::UniquePosition::After(
      p1, syncer::UniquePosition::RandomSuffix());
  const syncer::UniquePosition p3 = syncer::UniquePosition::After(
      p2, syncer::UniquePosition::RandomSuffix());
  // "c" moved to the front: a, b keep theirs, c gets one before a.
  const std::vector<std::string> keys = {"c", "new", "a", "b"};
  const std::vector<syncer::UniquePosition> positions =
      internal::AssignPositions(keys, {p3, std::nullopt, p1, p2});
  ASSERT_EQ(positions.size(), 4u);
  EXPECT_TRUE(positions[2].Equals(p1));
  EXPECT_TRUE(positions[3].Equals(p2));
  for (size_t i = 1; i < positions.size(); ++i) {
    EXPECT_TRUE(positions[i - 1].LessThan(positions[i])) << i;
  }
}

TEST_F(CompassSpacesSyncBridgeTest, CommitsNothingBeforeTheInitialSync) {
  std::unique_ptr<Device> a = MakeDevice();
  EXPECT_CALL(a->processor(), Put).Times(0);
  Populate(*a);
  a->bridge().SendLocalChangesForTesting();
  task_environment_.RunUntilIdle();
}

TEST_F(CompassSpacesSyncBridgeTest, FirstDeviceUploadsPinnedAndFavorites) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();

  // One space, a folder, two pinned tabs, a favorite; not the Today tab.
  EXPECT_EQ(server_.entities.size(), 5u);
  SpaceModel& space = a->space(0);
  const sync_pb::CompassSpace& synced_space =
      server_.entities[Key(space.id())].space();
  EXPECT_EQ(synced_space.name(), "Main");
  EXPECT_EQ(synced_space.color(), static_cast<int>(SpaceColor::kBlue) + 1);
  EXPECT_EQ(synced_space.archive_after_seconds(),
            kDefaultArchiveAfter.InSeconds());
  EXPECT_TRUE(synced_space.has_directory_position());

  SidebarNode* work = FindByTitle(*space.pinned_root(), u"Work");
  SidebarNode* mail = FindByTitle(*space.pinned_root(), u"Mail");
  const sync_pb::CompassNode& folder = server_.entities[Key(work->id())].node();
  EXPECT_EQ(folder.type(), kNodeFolder);
  EXPECT_EQ(folder.section(), kSectionPinned);
  EXPECT_EQ(folder.space_uuid(), Key(space.id()));
  EXPECT_TRUE(folder.parent_uuid().empty());
  const sync_pb::CompassNode& tab = server_.entities[Key(mail->id())].node();
  EXPECT_EQ(tab.type(), kNodeTab);
  EXPECT_EQ(tab.parent_uuid(), Key(work->id()));
  EXPECT_EQ(tab.url(), "https://mail.test/");
  EXPECT_EQ(tab.title(), "Mail");

  const SidebarNode* fav = a->favorites().pinned_root()->children()[0].get();
  const sync_pb::CompassNode& favorite =
      server_.entities[Key(fav->id())].node();
  EXPECT_EQ(favorite.section(), kSectionFavorites);
  EXPECT_TRUE(favorite.space_uuid().empty());

  // Nothing more to send.
  EXPECT_EQ(a->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, SecondDeviceGetsTheTreeWithoutEcho) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->space(0).SetCollapsed(FindByTitle(*a->space(0).pinned_root(), u"Work"),
                           true);
  a->Join();

  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // B's untouched "Home" gave way to the chain's space.
  EXPECT_EQ(Describe(b->list()), Describe(a->list()));
  EXPECT_EQ(Describe(b->list()), "Main/1(Work-{Mail},News) fav(Fav)");
  // Same IDs.
  SidebarNode* mail_a = FindByTitle(*a->space(0).pinned_root(), u"Mail");
  EXPECT_TRUE(b->space(0).FindById(mail_a->id()));
  EXPECT_EQ(b->space(0).id(), a->space(0).id());
  // Applying didn't send anything back.
  EXPECT_EQ(b->last_push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, RenamesMovesAndCollapseFlowBothWays) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // A renames the folder and moves News before it.
  SpaceModel& space_a = a->space(0);
  SidebarNode* work = FindByTitle(*space_a.pinned_root(), u"Work");
  space_a.SetTitle(work, u"Job");
  SidebarNode* news = FindByTitle(*space_a.pinned_root(), u"News");
  space_a.Move(news, space_a.pinned_root(), 0);
  // One entity: News keeps its position, the folder gets a new one (and its
  // new name).
  EXPECT_EQ(a->Push(), 1u);
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "News,Job{Mail}");
  EXPECT_EQ(b->last_push(), 0u);

  // B moves Mail out of the folder, to the end, and collapses the folder.
  SpaceModel& space_b = b->space(0);
  SidebarNode* mail = FindByTitle(*space_b.pinned_root(), u"Mail");
  space_b.Move(mail, space_b.pinned_root(), 2);
  space_b.SetCollapsed(FindByTitle(*space_b.pinned_root(), u"Job"), true);
  // One move, one collapse: only those two entities.
  EXPECT_EQ(b->Push(), 2u);
  a->Pull();
  EXPECT_EQ(Describe(*a->space(0).pinned_root()), "News,Job-{},Mail");
  EXPECT_EQ(a->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, SpacesSyncWithTheirOrder) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  SpaceModel* second = a->list().AddSpace(1, u"Second");
  second->SetColor(SpaceColor::kGreen);
  second->SetArchiveAfter(base::TimeDelta());
  a->Push();
  b->Pull();
  ASSERT_EQ(b->list().spaces().size(), 2u);
  EXPECT_EQ(b->space(1).name(), u"Second");
  EXPECT_EQ(b->space(1).color(), SpaceColor::kGreen);
  EXPECT_TRUE(b->space(1).archive_after().is_zero());

  // B puts it first.
  b->list().MoveSpace(b->space(1), 0);
  EXPECT_EQ(b->Push(), 1u);
  a->Pull();
  EXPECT_EQ(a->space(0).name(), u"Second");
  EXPECT_EQ(a->space(1).name(), u"Main");

  // A deletes it.
  a->list().RemoveSpace(a->space(0));
  a->Push();
  b->Pull();
  ASSERT_EQ(b->list().spaces().size(), 1u);
  EXPECT_EQ(b->space(0).name(), u"Main");
}

TEST_F(CompassSpacesSyncBridgeTest, DeletesIncludingSubtrees) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // B deletes the pinned tab and the folder with its tab.
  SpaceModel& space_b = b->space(0);
  space_b.Remove(FindByTitle(*space_b.pinned_root(), u"News"));
  space_b.Remove(FindByTitle(*space_b.pinned_root(), u"Work"));
  EXPECT_EQ(b->Push(), 3u);  // News, Work, Mail.
  a->Pull();
  EXPECT_EQ(Describe(*a->space(0).pinned_root()), "");
  // Today isn't touched.
  EXPECT_EQ(Describe(*a->space(0).today_root()), "Today tab");
  EXPECT_EQ(a->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, KeepsEntriesAddedToAFolderDeletedElsewhere) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // A adds a tab to Work (not yet sent) while B deletes Work.
  SpaceModel& space_a = a->space(0);
  space_a.AddTab(FindByTitle(*space_a.pinned_root(), u"Work"), 1,
                 GURL("https://late.test/"), u"Late");
  SpaceModel& space_b = b->space(0);
  space_b.Remove(FindByTitle(*space_b.pinned_root(), u"Work"));
  b->Push();
  a->Pull();
  EXPECT_EQ(Describe(*a->space(0).pinned_root()), "News,Late");
  a->Push();
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "News,Late");
}

TEST_F(CompassSpacesSyncBridgeTest, ParksAChildUntilItsParentArrives) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  SpaceModel& space_a = a->space(0);
  SidebarNode* folder = space_a.AddFolder(space_a.pinned_root(), 2, u"New");
  SidebarNode* child =
      space_a.AddTab(folder, 0, GURL("https://child.test/"), u"Child");
  a->Push();
  const std::string folder_key = Key(folder->id());
  const std::string child_key = Key(child->id());

  // B gets the child first.
  std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
      latest = {{child_key, server_.entities[child_key]}};
  b->ApplyRemote({child_key}, latest);
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "Work{Mail},News,Child");
  // Parked: nothing is committed for it.
  EXPECT_EQ(b->Push(), 0u);

  latest = {{folder_key, server_.entities[folder_key]}};
  b->ApplyRemote({folder_key}, latest);
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "Work{Mail},News,New{Child}");
  EXPECT_EQ(b->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, ConcurrentMovesIntoEachOtherConverge) {
  std::unique_ptr<Device> a = MakeDevice();
  SpaceModel& space_a = a->space(0);
  SidebarNode* x = space_a.AddFolder(space_a.pinned_root(), 0, u"X");
  space_a.AddTab(x, 0, GURL("https://x.test/"), u"InX");
  SidebarNode* y = space_a.AddFolder(space_a.pinned_root(), 1, u"Y");
  space_a.AddTab(y, 0, GURL("https://y.test/"), u"InY");
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // A moves X into Y; B moves Y into X; both commit before seeing the other.
  space_a.Move(x, y, 1);
  SpaceModel& space_b = b->space(0);
  space_b.Move(FindByTitle(*space_b.pinned_root(), u"Y"),
               FindByTitle(*space_b.pinned_root(), u"X"), 1);
  a->Push();
  b->Push();
  a->Pull();
  b->Pull();
  // The fix-ups each made go round once more.
  a->Pull();
  b->Pull();
  a->Pull();

  const std::string tree_a = Describe(*a->space(0).pinned_root());
  EXPECT_EQ(tree_a, Describe(*b->space(0).pinned_root()));
  // Nothing lost: both folders and both tabs are there.
  for (const char16_t* title : {u"X", u"Y", u"InX", u"InY"}) {
    EXPECT_TRUE(FindByTitle(*a->space(0).pinned_root(), title))
        << base::UTF16ToUTF8(title);
  }
  EXPECT_EQ(a->Push(), 0u);
  EXPECT_EQ(b->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, InvalidPlacementGoesToTheRoot) {
  std::unique_ptr<Device> a = MakeDevice();
  SpaceModel& space_a = a->space(0);
  SidebarNode* left = space_a.AddTab(space_a.pinned_root(), 0,
                                     GURL("https://l.test/"), u"L");
  SidebarNode* right = space_a.AddTab(space_a.pinned_root(), 1,
                                      GURL("https://r.test/"), u"R");
  SidebarNode* split = space_a.Split(left, right);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "[L|R]");

  // Another client puts a folder inside the split.
  sync_pb::CompassSpacesSpecifics folder;
  const std::string key = Key(base::Uuid::GenerateRandomV4());
  folder.mutable_node()->set_uuid(key);
  folder.mutable_node()->set_space_uuid(Key(space_a.id()));
  folder.mutable_node()->set_parent_uuid(Key(split->id()));
  folder.mutable_node()->set_section(kSectionPinned);
  folder.mutable_node()->set_type(kNodeFolder);
  folder.mutable_node()->set_title("Bad");
  std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
      latest = {{key, folder}};
  b->ApplyRemote({key}, latest);
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "[L|R],Bad{}");
  // The fix-up is committed.
  b->Push();
  EXPECT_TRUE(server_.entities[key].node().parent_uuid().empty());
}

TEST_F(CompassSpacesSyncBridgeTest, UnpinningIsADeleteElsewhere) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  SpaceModel& space_a = a->space(0);
  space_a.Move(FindByTitle(*space_a.pinned_root(), u"News"),
               space_a.today_root(), 0);
  EXPECT_EQ(a->Push(), 1u);
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "Work{Mail}");

  // A later client that syncs Today moves it there: it goes to Today here.
  SidebarNode* mail = FindByTitle(*b->space(0).pinned_root(), u"Mail");
  sync_pb::CompassSpacesSpecifics today = server_.entities[Key(mail->id())];
  today.mutable_node()->set_section(kSectionToday);
  today.mutable_node()->clear_parent_uuid();
  std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
      latest = {{Key(mail->id()), today}};
  b->ApplyRemote({Key(mail->id())}, latest);
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "Work{}");
  EXPECT_EQ(Describe(*b->space(0).today_root()), "Mail");
  // Not deleted from the server: it's Today's now.
  EXPECT_EQ(b->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, RemoteUrlAndTitleChangesApply) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  SpaceModel& space_a = a->space(0);
  SidebarNode* news = FindByTitle(*space_a.pinned_root(), u"News");
  space_a.SetUrl(news, GURL("https://news.test/today"));
  space_a.SetCustomTitle(news, u"Paper");
  a->Push();
  b->Pull();
  SidebarNode* news_b = b->space(0).FindById(news->id());
  ASSERT_TRUE(news_b);
  EXPECT_EQ(news_b->url(), GURL("https://news.test/today"));
  EXPECT_EQ(news_b->custom_title(), u"Paper");
  EXPECT_EQ(b->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, FavoritesMoveBetweenSections) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // A makes News a favorite.
  SpaceModel& space_a = a->space(0);
  SidebarNode* news = FindByTitle(*space_a.pinned_root(), u"News");
  space_a.MoveToSpace(news, a->favorites(), a->favorites().pinned_root(), 1);
  EXPECT_EQ(a->Push(), 1u);
  b->Pull();
  EXPECT_EQ(Describe(b->list()), "Main/1(Work{Mail}) fav(Fav,News)");
}

TEST_F(CompassSpacesSyncBridgeTest, MergeIsAUnion) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();

  // C has its own space, and the same favorite as the chain.
  std::unique_ptr<Device> c = MakeDevice();
  SpaceModel& own = c->space(0);
  own.SetName(u"Mine");
  own.AddTab(own.pinned_root(), 0, GURL("https://mine.test/"), u"MyTab");
  c->favorites().AddTab(c->favorites().pinned_root(), 0,
                        GURL("https://fav.test/"), u"Fav");
  c->favorites().AddTab(c->favorites().pinned_root(), 1,
                        GURL("https://other.test/"), u"Other");
  c->Join();

  // The chain's space first, C's after; favorites without the duplicate.
  EXPECT_EQ(Describe(c->list()),
            "Main/1(Work{Mail},News) Mine/0(MyTab) fav(Fav,Other)");
  a->Pull();
  EXPECT_EQ(Describe(a->list()), Describe(c->list()));
}

TEST_F(CompassSpacesSyncBridgeTest, NodesWaitForTheirSpace) {
  std::unique_ptr<Device> a = MakeDevice();
  SpaceModel* extra = a->list().AddSpace(1, u"Extra");
  extra->AddTab(extra->pinned_root(), 0, GURL("https://e.test/"), u"E");
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // A node arrives before its (new) space.
  SpaceModel* later = a->list().AddSpace(2, u"Later");
  SidebarNode* tab =
      later->AddTab(later->pinned_root(), 0, GURL("https://l.test/"), u"L");
  a->Push();
  const std::string tab_key = Key(tab->id());
  const std::string space_key = Key(later->id());
  std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
      latest = {{tab_key, server_.entities[tab_key]}};
  b->ApplyRemote({tab_key}, latest);
  // Not applied, and not deleted either.
  EXPECT_EQ(b->Push(), 0u);

  latest = {{space_key, server_.entities[space_key]}};
  b->ApplyRemote({space_key}, latest);
  ASSERT_EQ(b->list().spaces().size(), 3u);
  EXPECT_EQ(Describe(*b->space(2).pinned_root()), "L");
  EXPECT_EQ(b->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, KeepsFieldsItDoesntKnow) {
  std::unique_ptr<Device> a = MakeDevice();
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // Another client set an icon and a profile.
  const std::string key = Key(b->space(0).id());
  sync_pb::CompassSpacesSpecifics space = server_.entities[key];
  space.mutable_space()->set_icon("star");
  space.mutable_space()->set_profile_uuid("p");
  std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
      latest = {{key, space}};
  b->ApplyRemote({key}, latest);
  b->space(0).SetName(u"Renamed");
  EXPECT_EQ(b->Push(), 1u);
  EXPECT_EQ(server_.entities[key].space().icon(), "star");
  EXPECT_EQ(server_.entities[key].space().profile_uuid(), "p");
  EXPECT_EQ(server_.entities[key].space().name(), "Renamed");
}

TEST_F(CompassSpacesSyncBridgeTest, RestartSendsNothing) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();

  a->StartBridge();
  EXPECT_EQ(a->Push(), 0u);

  // A change made while the bridge wasn't running is sent once it is.
  SpaceModel& space = a->space(0);
  space.Remove(FindByTitle(*space.pinned_root(), u"News"));
  a->StartBridge();
  EXPECT_EQ(a->Push(), 1u);
}

TEST_F(CompassSpacesSyncBridgeTest, DisablingSyncKeepsTheSpaces) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  a->bridge().ApplyDisableSyncChanges(a->bridge().CreateMetadataChangeList());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Describe(a->list()), "Main/1(Work{Mail},News) fav(Fav)");
}

TEST_F(CompassSpacesSyncBridgeTest, ValidatesEntities) {
  std::unique_ptr<Device> a = MakeDevice();
  syncer::EntityData data;
  EXPECT_FALSE(a->bridge().IsEntityDataValid(data));
  data.specifics.mutable_compass_spaces()->mutable_node()->set_uuid("nope");
  EXPECT_FALSE(a->bridge().IsEntityDataValid(data));
  const std::string key = Key(base::Uuid::GenerateRandomV4());
  data.specifics.mutable_compass_spaces()->mutable_node()->set_uuid(key);
  EXPECT_TRUE(a->bridge().IsEntityDataValid(data));
  EXPECT_EQ(a->bridge().GetStorageKey(data), key);
  EXPECT_EQ(a->bridge().GetClientTag(data), key);
}

}  // namespace
}  // namespace compass::sync
