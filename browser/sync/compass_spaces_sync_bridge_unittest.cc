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
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "components/sync/base/client_tag_hash.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/deletion_origin.h"
#include "components/sync/model/conflict_resolution.h"
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

// A space's color: its place in the palette (0 grey, 1 blue...), or "#rgb".
std::string DescribeColor(SpaceColor color) {
  for (size_t i = 0; i < kSpaceColorPalette.size(); ++i) {
    if (kSpaceColorPalette[i].color == color) {
      return base::NumberToString(i);
    }
  }
  return base::StringPrintf("#%06x", *color.tint() & 0xFFFFFF);
}

// Spaces (name, color, pinned tree) in order, then the favorites.
std::string Describe(const SpaceList& list) {
  std::string result;
  for (const auto& space : list.spaces()) {
    result += base::UTF16ToUTF8(space->name()) + "/" +
              DescribeColor(space->color()) + "(" +
              Describe(*space->pinned_root()) + ") ";
  }
  return result + "fav(" + Describe(*list.favorites().pinned_root()) + ")";
}

// |space|'s archive, newest first: titles, a split's as "[a|b]".
std::string DescribeArchive(const SpaceModel& space) {
  std::string result;
  for (const ArchivedEntry& entry : space.archive()) {
    if (!result.empty()) {
      result += ",";
    }
    std::string tabs;
    for (const ArchivedEntry::Tab& tab : entry.tabs) {
      tabs += (tabs.empty() ? "" : "|") + base::UTF16ToUTF8(tab.title);
    }
    result += entry.tabs.size() > 1 ? "[" + tabs + "]" : tabs;
  }
  return result;
}

bool KeepNothing(const SidebarNode&) {
  return false;
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

// What windows would answer: which entries the focused window shows, which
// are in use, and live tabs handed from one entry to another.
class TestDelegate : public CompassSpacesSyncBridge::Delegate {
 public:
  explicit TestDelegate(SpaceList* list) : list_(list) {}

  void DeleteSpace(SpaceModel& space) override {
    if (list_->spaces().size() > 1) {
      list_->RemoveSpace(space);
    }
  }
  void MoveEntry(SidebarNode* node,
                 SpaceModel& target,
                 SidebarNode* parent,
                 size_t index) override {
    SpaceModel* source = list_->favorites().Contains(node)
                             ? &list_->favorites()
                             : list_->FindSpaceOf(node);
    source->MoveToSpace(node, target, parent, index);
  }
  bool IsShownInFocusedWindow(const SidebarNode& entry) override {
    return Contains(focused, entry);
  }
  bool IsInUse(const SidebarNode& entry) override {
    return Contains(in_use, entry);
  }
  void TransferLiveTab(const SidebarNode& from, SidebarNode& to) override {
    transfers.emplace_back(from.id(), to.id());
  }
  void ApplyFavicons(
      const std::vector<sync_pb::CompassFavicon>& icons) override {
    for (const sync_pb::CompassFavicon& icon : icons) {
      favicons[icon.host()] = icon.png();
    }
  }
  void OnFaviconsWritable() override { ++favicons_writable; }
  std::optional<std::vector<sync_pb::CompassMemory>> GetLocalMemories()
      override {
    if (!memories) {
      return std::nullopt;
    }
    std::vector<sync_pb::CompassMemory> list;
    for (const auto& [key, memory] : *memories) {
      list.push_back(memory);
    }
    return list;
  }
  void ApplyMemories(const std::vector<sync_pb::CompassMemory>& changed,
                     const std::set<std::string>& deleted) override {
    if (!memories) {
      return;
    }
    for (const sync_pb::CompassMemory& memory : changed) {
      auto it = memories->find(memory.uuid());
      if (it == memories->end() ||
          memory.update_time() >= it->second.update_time()) {
        (*memories)[memory.uuid()] = memory;
      }
    }
    for (const std::string& key : deleted) {
      memories->erase(key);
    }
  }

  // Claude's memories by id; nullopt: not loaded.
  std::optional<std::map<std::string, sync_pb::CompassMemory>> memories;
  // Synced icons handed over, by host: their PNG.
  std::map<std::string, std::string> favicons;
  int favicons_writable = 0;
  // Entries by ID, tabs included.
  std::set<base::Uuid> focused;
  std::set<base::Uuid> in_use;
  std::vector<std::pair<base::Uuid, base::Uuid>> transfers;

 private:
  static bool Contains(const std::set<base::Uuid>& ids,
                       const SidebarNode& entry) {
    if (ids.contains(entry.id())) {
      return true;
    }
    return std::ranges::any_of(entry.children(), [&ids](const auto& child) {
      return ids.contains(child->id());
    });
  }

  const raw_ptr<SpaceList> list_;
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
        &list_, &delegate_);
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
  // what that made it change (fix-ups; last_push() says how many). An
  // entity this device committed last (the server has its version) isn't
  // sent back to it.
  void Pull() {
    std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
        latest;
    std::map<std::string, int> last_from;
    std::vector<std::string> order;
    for (size_t i = cursor_; i < server_->log.size(); ++i) {
      const Server::Change& change = server_->log[i];
      last_from[change.key] = change.from;
      if (change.from == id_) {
        continue;
      }
      if (!latest.contains(change.key)) {
        order.push_back(change.key);
      }
      latest[change.key] = change.specifics;
    }
    std::erase_if(order, [&](const std::string& key) {
      return last_from[key] == id_;
    });
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
        syncer::COMPASS_SPACES,
        specifics.has_space()     ? specifics.space().uuid()
        : specifics.has_favicon() ? specifics.favicon().uuid()
        : specifics.has_memory()  ? specifics.memory().uuid()
                                  : specifics.node().uuid());
    return data;
  }

  SpaceList& list() { return list_; }
  TestDelegate& delegate() { return delegate_; }
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
  TestDelegate delegate_{&list_};
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

  // Mock time: commit throttling and the archive's age.
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
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

// Slice 7: a remote delete that meets a local change keeps the local entity
// (upstream's rule, now logged as "kept"); other conflicts take the remote.
TEST_F(CompassSpacesSyncBridgeTest, ConflictsKeepLocalOverRemoteDeletes) {
  std::unique_ptr<Device> a = MakeDevice();
  syncer::EntityData deleted;
  EXPECT_EQ(syncer::ConflictResolution::kUseLocal,
            a->bridge().ResolveConflict("some-uuid", deleted));
  syncer::EntityData changed;
  changed.specifics.mutable_compass_spaces()->mutable_node()->set_uuid(
      "some-uuid");
  EXPECT_EQ(syncer::ConflictResolution::kUseRemote,
            a->bridge().ResolveConflict("some-uuid", changed));
}

TEST_F(CompassSpacesSyncBridgeTest, CommitsNothingBeforeTheInitialSync) {
  std::unique_ptr<Device> a = MakeDevice();
  EXPECT_CALL(a->processor(), Put).Times(0);
  Populate(*a);
  a->bridge().SendLocalChangesForTesting();
  task_environment_.RunUntilIdle();
}

TEST_F(CompassSpacesSyncBridgeTest, FirstDeviceUploadsItsSidebar) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();

  // One space, a folder, two pinned tabs, the Today tab, a favorite.
  EXPECT_EQ(server_.entities.size(), 6u);
  const SidebarNode* today_tab =
      a->space(0).today_root()->children()[0].get();
  EXPECT_EQ(server_.entities[Key(today_tab->id())].node().section(),
            kSectionToday);
  SpaceModel& space = a->space(0);
  const sync_pb::CompassSpace& synced_space =
      server_.entities[Key(space.id())].space();
  EXPECT_EQ(synced_space.name(), "Main");
  // Blue is the palette's second color.
  EXPECT_EQ(synced_space.color(), 2);
  EXPECT_FALSE(synced_space.has_color_rgb());
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

TEST_F(CompassSpacesSyncBridgeTest, PinningAndUnpinningAreMoves) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // A unpins News: the same entity moves to Today.
  SpaceModel& space_a = a->space(0);
  SidebarNode* news = FindByTitle(*space_a.pinned_root(), u"News");
  space_a.Move(news, space_a.today_root(), 0);
  EXPECT_EQ(a->Push(), 1u);
  EXPECT_EQ(server_.entities[Key(news->id())].node().section(), kSectionToday);
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).pinned_root()), "Work{Mail}");
  EXPECT_EQ(Describe(*b->space(0).today_root()), "News,Today tab");
  EXPECT_EQ(b->last_push(), 0u);

  // B pins the Today tab: back the other way, at once.
  SpaceModel& space_b = b->space(0);
  space_b.Move(FindByTitle(*space_b.today_root(), u"Today tab"),
               space_b.pinned_root(), 1);
  EXPECT_EQ(b->Push(), 1u);
  a->Pull();
  EXPECT_EQ(Describe(*a->space(0).pinned_root()), "Work{Mail},Today tab");
  EXPECT_EQ(Describe(*a->space(0).today_root()), "News");
  EXPECT_EQ(a->last_push(), 0u);
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

  // Another client set an icon this version can't read, and a profile.
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

// Main's space themes: color (off the palette too), gradient, intensity,
// grain, appearance and icon go to other devices, and don't come back.
TEST_F(CompassSpacesSyncBridgeTest, SpaceThemesRoundTrip) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  SpaceModel& space = a->space(0);
  // Closest to blue.
  const SpaceColor custom = SpaceColor::FromRGB(40, 120, 230);
  const SpaceColor custom_stop = SpaceColor::FromRGB(1, 2, 3);
  space.SetColor(custom);
  space.SetGradient({SpaceColor::kPink, custom_stop});
  space.SetIntensity(0.8);
  space.SetGrain(0.25);
  space.SetAppearance(SpaceAppearance::kDark);
  space.SetIcon(*SpaceIcon::Emoji("\U0001F9ED"));
  EXPECT_EQ(a->Push(), 1u);

  const sync_pb::CompassSpace& synced = server_.entities[Key(space.id())].space();
  EXPECT_EQ(synced.color(), 2);
  EXPECT_EQ(synced.color_rgb(), 0x2878e6u);
  ASSERT_EQ(synced.gradient_size(), 2);
  EXPECT_EQ(synced.gradient(0).palette(), 4);
  EXPECT_FALSE(synced.gradient(0).has_rgb());
  EXPECT_EQ(synced.gradient(1).rgb(), 0x010203u);
  EXPECT_DOUBLE_EQ(synced.intensity(), 0.8);
  EXPECT_DOUBLE_EQ(synced.grain(), 0.25);
  EXPECT_EQ(synced.appearance(), 3);
  EXPECT_EQ(synced.icon(), "emoji:\U0001F9ED");

  b->Pull();
  SpaceModel& space_b = b->space(0);
  EXPECT_EQ(space_b.color(), custom);
  EXPECT_EQ(space_b.gradient(),
            (std::vector<SpaceColor>{SpaceColor::kPink, custom_stop}));
  EXPECT_DOUBLE_EQ(space_b.intensity(), 0.8);
  EXPECT_DOUBLE_EQ(space_b.grain(), 0.25);
  EXPECT_EQ(space_b.appearance(), SpaceAppearance::kDark);
  EXPECT_EQ(space_b.icon(), *SpaceIcon::Emoji("\U0001F9ED"));
  EXPECT_EQ(b->last_push(), 0u);
  EXPECT_EQ(Describe(b->list()), "Main/#2878e6(Work{Mail},News) fav(Fav)");

  // Back to the defaults, which leave the fields out.
  space_b.SetColor(SpaceColor::kGrey);
  space_b.SetIntensity(kDefaultSpaceIntensity);
  space_b.SetGrain(0);
  space_b.SetAppearance(SpaceAppearance::kAuto);
  space_b.SetIcon(*SpaceIcon::Symbol("star"));
  EXPECT_EQ(b->Push(), 1u);
  const sync_pb::CompassSpace& reset =
      server_.entities[Key(space.id())].space();
  EXPECT_EQ(reset.color(), 1);
  EXPECT_FALSE(reset.has_color_rgb());
  EXPECT_EQ(reset.gradient_size(), 0);
  EXPECT_FALSE(reset.has_intensity());
  EXPECT_FALSE(reset.has_grain());
  EXPECT_FALSE(reset.has_appearance());
  EXPECT_EQ(reset.icon(), "symbol:star");
  a->Pull();
  EXPECT_EQ(space.color(), SpaceColor::kGrey);
  EXPECT_TRUE(space.gradient().empty());
  EXPECT_EQ(space.intensity(), kDefaultSpaceIntensity);
  EXPECT_EQ(space.grain(), 0);
  EXPECT_EQ(space.appearance(), SpaceAppearance::kAuto);
  EXPECT_EQ(space.icon(), *SpaceIcon::Symbol("star"));
  EXPECT_EQ(a->last_push(), 0u);
}

// A client from before themes only knows `color`: its palette color
// applies, over a `color_rgb` it kept as received, and isn't rewritten.
// Invalid theme values are ignored or clamped.
TEST_F(CompassSpacesSyncBridgeTest, OldClientsPaletteColorApplies) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->space(0).SetColor(SpaceColor::FromRGB(40, 120, 230));
  a->space(0).SetGradient({SpaceColor::kPink});
  a->Join();

  const std::string key = Key(a->space(0).id());
  sync_pb::CompassSpacesSpecifics old = server_.entities[key];
  ASSERT_TRUE(old.space().has_color_rgb());
  // Red.
  old.mutable_space()->set_color(5);
  std::map<std::string, std::optional<sync_pb::CompassSpacesSpecifics>>
      latest = {{key, old}};
  a->ApplyRemote({key}, latest);
  EXPECT_EQ(a->space(0).color(), SpaceColor::kRed);
  EXPECT_EQ(a->space(0).gradient(),
            std::vector<SpaceColor>{SpaceColor::kPink});
  EXPECT_EQ(a->Push(), 0u);

  // An entity with nothing but a palette color: the theme's defaults.
  sync_pb::CompassSpacesSpecifics plain;
  plain.mutable_space()->set_uuid(key);
  plain.mutable_space()->set_name("Main");
  plain.mutable_space()->set_color(7);
  latest = {{key, plain}};
  a->ApplyRemote({key}, latest);
  EXPECT_EQ(a->space(0).color(), SpaceColor::kYellow);
  EXPECT_TRUE(a->space(0).gradient().empty());
  EXPECT_EQ(a->space(0).intensity(), kDefaultSpaceIntensity);

  // Out of range: an unknown color stays, intensity is clamped, a broken
  // gradient entry and an unknown appearance are ignored.
  sync_pb::CompassSpacesSpecifics invalid = plain;
  sync_pb::CompassSpace& theme = *invalid.mutable_space();
  theme.set_color(42);
  theme.add_gradient()->set_rgb(0x1000000);
  theme.add_gradient()->set_palette(3);
  theme.set_intensity(7);
  theme.set_appearance(9);
  latest = {{key, invalid}};
  a->ApplyRemote({key}, latest);
  EXPECT_EQ(a->space(0).color(), SpaceColor::kYellow);
  EXPECT_EQ(a->space(0).gradient(),
            std::vector<SpaceColor>{SpaceColor::kPurple});
  EXPECT_EQ(a->space(0).intensity(), 1);
  EXPECT_EQ(a->space(0).appearance(), SpaceAppearance::kAuto);
}

// Each theme setter alone schedules a commit of the space, by itself.
TEST_F(CompassSpacesSyncBridgeTest, ThemeOnlyChangesCommit) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  SpaceModel& space = a->space(0);
  const std::string key = Key(space.id());

  auto commits_alone = [&](auto change) {
    change();
    task_environment_.RunUntilIdle();
    EXPECT_EQ(a->puts().size(), 1u);
    EXPECT_TRUE(a->puts().contains(key));
    a->Push();
  };
  commits_alone([&] { space.SetGradient({SpaceColor::kGreen}); });
  commits_alone([&] { space.SetIntensity(0.5); });
  commits_alone([&] { space.SetGrain(0.4); });
  commits_alone([&] { space.SetAppearance(SpaceAppearance::kLight); });
  commits_alone([&] { space.SetIcon(*SpaceIcon::Symbol("book")); });
  commits_alone([&] { space.SetColor(SpaceColor::FromRGB(10, 20, 30)); });
  const sync_pb::CompassSpace& synced = server_.entities[key].space();
  EXPECT_EQ(synced.gradient(0).palette(), 8);
  EXPECT_DOUBLE_EQ(synced.intensity(), 0.5);
  EXPECT_DOUBLE_EQ(synced.grain(), 0.4);
  EXPECT_EQ(synced.appearance(), 2);
  EXPECT_EQ(synced.icon(), "symbol:book");
  EXPECT_EQ(synced.color_rgb(), 0x0a141eu);
  EXPECT_EQ(a->Push(), 0u);
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

// Today and the archive -------------------------------------------------------

TEST_F(CompassSpacesSyncBridgeTest, TodayShowsOnTheOtherDeviceUnloaded) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel& space_a = a->space(0);
  SidebarNode* tab = FindByTitle(*space_a.today_root(), u"Today tab");
  const base::Time seen = base::Time::Now() - base::Hours(1);
  space_a.SetLastActive(tab, seen);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // Same ID, URL and last-active time; it's an entry there (the bridge
  // knows no tabs: B's windows load it when it's clicked).
  EXPECT_EQ(Describe(*b->space(0).today_root()), "Today tab");
  SidebarNode* tab_b = b->space(0).FindById(tab->id());
  ASSERT_TRUE(tab_b);
  EXPECT_EQ(tab_b->url(), GURL("https://today.test/"));
  EXPECT_EQ(tab_b->last_active(), seen);
  EXPECT_EQ(b->last_push(), 0u);

  // Folders and splits in Today too.
  SidebarNode* folder = space_a.AddFolder(space_a.today_root(), 0, u"Trip");
  space_a.AddTab(folder, 0, GURL("https://hotel.test/"), u"Hotel");
  SidebarNode* left = space_a.AddTab(space_a.today_root(), 2,
                                     GURL("https://l.test/"), u"L");
  SidebarNode* right = space_a.AddTab(space_a.today_root(), 3,
                                      GURL("https://r.test/"), u"R");
  space_a.Split(left, right);
  EXPECT_EQ(a->Push(), 5u);  // The folder, Hotel, the split and its panes.
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).today_root()), "Trip{Hotel},Today tab,[L|R]");
  EXPECT_EQ(b->last_push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, NewTabPagesStayLocal) {
  std::unique_ptr<Device> a = MakeDevice();
  SpaceModel& space = a->space(0);
  SidebarNode* today = space.today_root();
  SidebarNode* new_tab =
      space.AddTab(today, 0, GURL("chrome://newtab/"), u"New Tab");
  space.AddTab(today, 1, GURL("about:blank"), u"Blank");
  SidebarNode* left =
      space.AddTab(today, 2, GURL("https://left.test/"), u"Left");
  SidebarNode* blank = space.AddTab(today, 3, GURL("about:blank"), u"Pane");
  space.Split(left, blank);
  a->Join();
  // Only the space: no blank page, nor a split with one.
  EXPECT_EQ(server_.entities.size(), 1u);

  // Once they show a page, they sync.
  space.SetUrl(new_tab, GURL("https://n.test/"));
  EXPECT_EQ(a->Push(), 1u);
  space.SetUrl(blank, GURL("https://right.test/"));
  EXPECT_EQ(a->Push(), 3u);
}

TEST_F(CompassSpacesSyncBridgeTest, ContentCommitsAtMostEveryTenSeconds) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  SpaceModel& space_a = a->space(0);
  SidebarNode* tab = FindByTitle(*space_a.today_root(), u"Today tab");
  const std::string key = Key(tab->id());

  // Redirects and title changes right after the last commit wait.
  task_environment_.FastForwardBy(base::Seconds(2));
  space_a.SetUrl(tab, GURL("https://today.test/2"));
  space_a.SetTitle(tab, u"Two");
  EXPECT_EQ(a->Push(), 0u);
  task_environment_.FastForwardBy(base::Seconds(3));
  space_a.SetTitle(tab, u"Three");
  EXPECT_EQ(a->Push(), 0u);
  // Ten seconds after it: the last state goes, once.
  task_environment_.FastForwardBy(base::Seconds(5));
  EXPECT_EQ(a->Push(), 1u);
  EXPECT_EQ(server_.entities[key].node().title(), "Three");
  EXPECT_EQ(server_.entities[key].node().url(), "https://today.test/2");
  b->Pull();
  EXPECT_EQ(b->space(0).FindById(tab->id())->title(), u"Three");
  // Applying it sent nothing back.
  EXPECT_EQ(b->last_push(), 0u);

  // Structure doesn't wait, and takes the content along.
  task_environment_.FastForwardBy(base::Seconds(1));
  space_a.SetTitle(tab, u"Four");
  EXPECT_EQ(a->Push(), 0u);
  space_a.Move(tab, space_a.pinned_root(), 0);
  EXPECT_EQ(a->Push(), 1u);
  EXPECT_EQ(server_.entities[key].node().title(), "Four");
  EXPECT_EQ(server_.entities[key].node().section(), kSectionPinned);
}

TEST_F(CompassSpacesSyncBridgeTest, NavigatingOnBothTheLastCommitWins) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  task_environment_.FastForwardBy(base::Seconds(10));
  SpaceModel& space_a = a->space(0);
  SpaceModel& space_b = b->space(0);
  SidebarNode* tab_a = FindByTitle(*space_a.today_root(), u"Today tab");
  SidebarNode* tab_b = space_b.FindById(tab_a->id());

  // B navigates twice; the second waits.
  space_b.SetUrl(tab_b, GURL("https://b.test/1"));
  EXPECT_EQ(b->Push(), 1u);
  task_environment_.FastForwardBy(base::Seconds(1));
  space_b.SetUrl(tab_b, GURL("https://b.test/2"));
  EXPECT_EQ(b->Push(), 0u);
  // A navigates meanwhile, and commits.
  space_a.SetUrl(tab_a, GURL("https://a.test/1"));
  EXPECT_EQ(a->Push(), 1u);

  // B's waiting navigation isn't overwritten: it goes next, and wins.
  b->Pull();
  EXPECT_EQ(tab_b->url(), GURL("https://b.test/2"));
  EXPECT_EQ(b->last_push(), 0u);
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_EQ(b->Push(), 1u);
  a->Pull();
  EXPECT_EQ(tab_a->url(), GURL("https://b.test/2"));
  // And it ends there: nothing goes back and forth.
  EXPECT_EQ(a->last_push(), 0u);
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_EQ(a->Push(), 0u);
  b->Pull();
  EXPECT_EQ(b->last_push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, ClosingAnEntryClosesItEverywhere) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  b->space(0).Remove(FindByTitle(*b->space(0).today_root(), u"Today tab"));
  EXPECT_EQ(b->Push(), 1u);
  a->Pull();
  EXPECT_EQ(Describe(*a->space(0).today_root()), "");
  EXPECT_EQ(a->last_push(), 0u);
  EXPECT_TRUE(a->delegate().transfers.empty());
}

TEST_F(CompassSpacesSyncBridgeTest, TheFocusedTabSurvivesACloseElsewhere) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  const base::Uuid old_id =
      FindByTitle(*a->space(0).today_root(), u"Today tab")->id();
  a->delegate().focused.insert(old_id);

  b->space(0).Remove(FindByTitle(*b->space(0).today_root(), u"Today tab"));
  b->Push();
  a->Pull();

  // Still there, with its live tab, under a new ID (the server doesn't take
  // a deleted one back), and sent again.
  EXPECT_EQ(Describe(*a->space(0).today_root()), "Today tab");
  const SidebarNode* kept = a->space(0).today_root()->children()[0].get();
  EXPECT_NE(kept->id(), old_id);
  ASSERT_EQ(a->delegate().transfers.size(), 1u);
  EXPECT_EQ(a->delegate().transfers[0], std::make_pair(old_id, kept->id()));
  EXPECT_EQ(a->last_push(), 1u);
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).today_root()), "Today tab");
  EXPECT_TRUE(b->space(0).FindById(kept->id()));
  EXPECT_EQ(b->last_push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, QuittingDoesntEmptyTodayElsewhere) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel& space_a = a->space(0);
  space_a.AddTab(space_a.today_root(), 1, GURL("chrome://newtab/"),
                 u"New Tab");
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // Quitting: the windows close, Today keeps its entries unloaded except the
  // new tab page (TabStripBridge), and the bridge goes with the profile.
  space_a.Remove(FindByTitle(*space_a.today_root(), u"New Tab"));
  a->StartBridge();
  EXPECT_EQ(a->Push(), 0u);
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).today_root()), "Today tab");
}

TEST_F(CompassSpacesSyncBridgeTest, ArchivingIsAMoveToTheArchive) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  SpaceModel& space_a = a->space(0);
  SidebarNode* tab = FindByTitle(*space_a.today_root(), u"Today tab");
  const base::Uuid id = tab->id();
  const base::Time archived = base::Time::Now();
  space_a.Archive(tab, archived);
  EXPECT_EQ(a->Push(), 1u);
  const sync_pb::CompassNode& synced = server_.entities[Key(id)].node();
  EXPECT_EQ(synced.section(), kSectionArchive);
  EXPECT_EQ(synced.archived_time(),
            archived.ToDeltaSinceWindowsEpoch().InMicroseconds());
  EXPECT_EQ(synced.url(), "https://today.test/");

  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).today_root()), "");
  ASSERT_EQ(b->space(0).archive().size(), 1u);
  EXPECT_EQ(b->space(0).archive()[0].id, id);
  EXPECT_EQ(b->space(0).archive()[0].archived_at, archived);
  EXPECT_EQ(DescribeArchive(b->space(0)), "Today tab");
  EXPECT_EQ(b->last_push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, ArchivingOnBothDevicesIsHarmless) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  // Both timers archive it, a second apart, before either hears of the
  // other.
  a->space(0).Archive(FindByTitle(*a->space(0).today_root(), u"Today tab"),
                      base::Time::Now());
  task_environment_.FastForwardBy(base::Seconds(1));
  b->space(0).Archive(FindByTitle(*b->space(0).today_root(), u"Today tab"),
                      base::Time::Now());
  a->Push();
  b->Push();
  a->Pull();
  b->Pull();

  EXPECT_EQ(DescribeArchive(a->space(0)), "Today tab");
  EXPECT_EQ(DescribeArchive(b->space(0)), "Today tab");
  EXPECT_EQ(a->space(0).archive()[0].archived_at,
            b->space(0).archive()[0].archived_at);
  EXPECT_EQ(a->Push(), 0u);
  EXPECT_EQ(b->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, ArchivedSplitsAndRestoring) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel& space_a = a->space(0);
  SidebarNode* left = space_a.AddTab(space_a.today_root(), 1,
                                     GURL("https://l.test/"), u"L");
  SidebarNode* right = space_a.AddTab(space_a.today_root(), 2,
                                      GURL("https://r.test/"), u"R");
  SidebarNode* split = space_a.Split(left, right);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  space_a.Archive(split, base::Time::Now());
  EXPECT_EQ(a->Push(), 3u);  // The split and its panes.
  b->Pull();
  EXPECT_EQ(DescribeArchive(b->space(0)), "[L|R]");
  EXPECT_EQ(Describe(*b->space(0).today_root()), "Today tab");
  EXPECT_EQ(b->last_push(), 0u);

  // B restores it: a new entry in Today, and gone from every archive.
  ASSERT_TRUE(b->space(0).Unarchive(b->space(0).archive()[0].id, 0));
  EXPECT_EQ(b->Push(), 6u);  // Three new, three deleted.
  a->Pull();
  EXPECT_EQ(DescribeArchive(a->space(0)), "");
  EXPECT_EQ(Describe(*a->space(0).today_root()), "[L|R],Today tab");
  EXPECT_EQ(a->last_push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, OldArchivedEntriesLeaveOnlyTheServer) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  SidebarNode* tab = FindByTitle(*a->space(0).today_root(), u"Today tab");
  const std::string key = Key(tab->id());
  a->space(0).Archive(tab, base::Time::Now());
  a->Push();
  b->Pull();
  ASSERT_EQ(DescribeArchive(b->space(0)), "Today tab");

  // A month on, the entry leaves the server (both devices delete it), and
  // stays in both archives.
  task_environment_.FastForwardBy(kArchiveSyncPeriod + base::Minutes(1));
  a->Push();
  b->Push();
  EXPECT_FALSE(server_.entities.contains(key));
  a->Pull();
  b->Pull();
  EXPECT_EQ(DescribeArchive(a->space(0)), "Today tab");
  EXPECT_EQ(DescribeArchive(b->space(0)), "Today tab");
  EXPECT_EQ(a->Push(), 0u);
  EXPECT_EQ(b->Push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, ArchivingGoesByTheLatestUseAnywhere) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel& space_a = a->space(0);
  SidebarNode* tab = FindByTitle(*space_a.today_root(), u"Today tab");
  space_a.SetLastActive(tab, base::Time::Now() - base::Hours(13));
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  task_environment_.FastForwardBy(base::Seconds(10));

  // B used it an hour ago: A's timer leaves it.
  SidebarNode* tab_b = b->space(0).FindById(tab->id());
  b->space(0).SetLastActive(tab_b, base::Time::Now() - base::Hours(1));
  EXPECT_EQ(b->Push(), 1u);
  a->Pull();
  EXPECT_EQ(space_a.ArchiveIdleEntries(base::Time::Now(), &KeepNothing), 0u);
  EXPECT_EQ(a->last_push(), 0u);

  // Unseen anywhere for the delay: archived, on both.
  task_environment_.FastForwardBy(kDefaultArchiveAfter);
  EXPECT_EQ(space_a.ArchiveIdleEntries(base::Time::Now(), &KeepNothing), 1u);
  a->Push();
  b->Pull();
  EXPECT_EQ(DescribeArchive(b->space(0)), "Today tab");
  EXPECT_EQ(Describe(*b->space(0).today_root()), "");
}

TEST_F(CompassSpacesSyncBridgeTest, AnEntryInUseStaysWhenArchivedElsewhere) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  SidebarNode* tab_a = FindByTitle(*a->space(0).today_root(), u"Today tab");
  const std::string key = Key(tab_a->id());
  a->delegate().in_use.insert(tab_a->id());

  b->space(0).Archive(FindByTitle(*b->space(0).today_root(), u"Today tab"),
                      base::Time::Now());
  b->Push();
  a->Pull();
  // Kept, seen now, and back in Today on the server.
  EXPECT_EQ(Describe(*a->space(0).today_root()), "Today tab");
  EXPECT_EQ(DescribeArchive(a->space(0)), "");
  EXPECT_EQ(tab_a->last_active(), base::Time::Now());
  EXPECT_EQ(a->last_push(), 1u);
  EXPECT_EQ(server_.entities[key].node().section(), kSectionToday);
  b->Pull();
  EXPECT_EQ(Describe(*b->space(0).today_root()), "Today tab");
  EXPECT_EQ(DescribeArchive(b->space(0)), "");
  EXPECT_EQ(b->last_push(), 0u);
}

TEST_F(CompassSpacesSyncBridgeTest, MergeAddsTodayWithoutDuplicates) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();

  // C's untouched Home has the chain's Today page, and one of its own.
  std::unique_ptr<Device> c = MakeDevice();
  SpaceModel& home = c->space(0);
  const base::Uuid dup_id =
      home.AddTab(home.today_root(), 0, GURL("https://today.test/"), u"Dup")
          ->id();
  home.AddTab(home.today_root(), 1, GURL("https://own.test/"), u"Own");
  c->Join();

  EXPECT_EQ(Describe(*c->space(0).today_root()), "Today tab,Own");
  // A live tab of the duplicate would go to the chain's entry.
  ASSERT_EQ(c->delegate().transfers.size(), 1u);
  EXPECT_EQ(c->delegate().transfers[0].first, dup_id);
  a->Pull();
  EXPECT_EQ(Describe(*a->space(0).today_root()), "Today tab,Own");
  EXPECT_EQ(a->last_push(), 0u);
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

// docs/design/sync.md, "Favicons": the same vectors as iOS's FaviconKeyTests.
TEST(CompassFaviconKeyTest, MatchesTheSpec) {
  EXPECT_EQ(FaviconKey("www.example.com"),
            "c94c25bc-f583-5f0a-88eb-63e59b454926");
  EXPECT_EQ(FaviconKey("github.com"), "b73d689b-d088-5dec-9f35-e41fb59757a0");
  EXPECT_EQ(FaviconKey("GitHub.com"), FaviconKey("github.com"));
}

sync_pb::CompassSpacesSpecifics FaviconSpecifics(const std::string& host,
                                                 const std::string& png) {
  sync_pb::CompassSpacesSpecifics specifics;
  sync_pb::CompassFavicon* favicon = specifics.mutable_favicon();
  favicon->set_uuid(FaviconKey(host));
  favicon->set_host(host);
  favicon->set_png(png);
  favicon->set_source_size(32);
  favicon->set_update_time(
      base::Time::Now().ToDeltaSinceWindowsEpoch().InMicroseconds());
  return specifics;
}

TEST_F(CompassSpacesSyncBridgeTest, FaviconsAreValidOnlyKeyedByTheirHost) {
  std::unique_ptr<Device> a = MakeDevice();
  syncer::EntityData data =
      Device::Data(FaviconSpecifics("news.test", "png"));
  EXPECT_TRUE(a->bridge().IsEntityDataValid(data));
  EXPECT_EQ(a->bridge().GetStorageKey(data), FaviconKey("news.test"));
  data.specifics.mutable_compass_spaces()->mutable_favicon()->set_uuid(
      FaviconKey("other.test"));
  EXPECT_FALSE(a->bridge().IsEntityDataValid(data));
  data.specifics.mutable_compass_spaces()->mutable_favicon()->clear_host();
  EXPECT_FALSE(a->bridge().IsEntityDataValid(data));
}

// Written only for hosts of entries; a larger icon replaces the account's,
// a different one of the same size only once it's a week old.
TEST_F(CompassSpacesSyncBridgeTest, OffersFaviconsByTheRules) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  EXPECT_FALSE(a->bridge().OfferFavicon("news.test", "png16", 16))
      << "not syncing yet";
  a->Join();
  EXPECT_EQ(a->delegate().favicons_writable, 1);
  const std::string key = FaviconKey("news.test");

  EXPECT_FALSE(a->bridge().OfferFavicon("other.test", "png16", 16));
  EXPECT_FALSE(a->bridge().OfferFavicon("news.test", "", 16));
  EXPECT_FALSE(a->bridge().OfferFavicon(
      "news.test", std::string(kFaviconMaxBytes + 1, 'x'), 16));
  // Favorites count; hosts are matched lowercase.
  EXPECT_TRUE(a->bridge().OfferFavicon("FAV.test", "fav", 16));
  EXPECT_TRUE(a->bridge().OfferFavicon("news.test", "png16", 16));
  a->Push();
  ASSERT_TRUE(server_.entities.contains(key));
  EXPECT_EQ(server_.entities[key].favicon().host(), "news.test");
  EXPECT_EQ(server_.entities[key].favicon().source_size(), 16);

  EXPECT_FALSE(a->bridge().OfferFavicon("news.test", "png8", 8));
  EXPECT_FALSE(a->bridge().OfferFavicon("news.test", "png16", 16));
  EXPECT_TRUE(a->bridge().OfferFavicon("news.test", "png32", 32));
  EXPECT_FALSE(a->bridge().OfferFavicon("news.test", "other32", 32));
  task_environment_.FastForwardBy(kFaviconRefreshAge);
  EXPECT_TRUE(a->bridge().OfferFavicon("news.test", "other32", 32));
  a->Push();
  EXPECT_EQ(server_.entities[key].favicon().png(), "other32");
}

// Another device shows the account's icons; nobody deletes them for not being
// in the sidebar, only once no entry points at the host for a day.
TEST_F(CompassSpacesSyncBridgeTest, FaviconsReachOtherDevicesAndAreCollected) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->Join();
  ASSERT_TRUE(a->bridge().OfferFavicon("news.test", "png", 16));
  a->Push();
  const std::string key = FaviconKey("news.test");

  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  EXPECT_EQ(b->delegate().favicons["news.test"], "png");
  EXPECT_TRUE(server_.entities.contains(key));
  b->Pull();
  a->Pull();
  EXPECT_TRUE(server_.entities.contains(key));

  // A later icon reaches B as a change.
  task_environment_.FastForwardBy(kFaviconRefreshAge);
  ASSERT_TRUE(a->bridge().OfferFavicon("news.test", "newer", 16));
  a->Push();
  b->Pull();
  EXPECT_EQ(b->delegate().favicons["news.test"], "newer");

  // No entry on the host: kept for a day, then deleted.
  SpaceModel& space = a->space(0);
  space.Remove(FindByTitle(*space.pinned_root(), u"News"));
  a->Push();
  EXPECT_TRUE(server_.entities.contains(key));
  task_environment_.FastForwardBy(kFaviconGracePeriod + base::Minutes(1));
  a->Push();
  EXPECT_FALSE(server_.entities.contains(key));
}


// Live folders (docs/design/live-folders.md, "Sync"): the folder and its
// source sync; its entries are made on each device and never do.

SidebarNode* AddLiveFolderWithEntries(SpaceModel& space,
                                      std::vector<const char*> keys) {
  SidebarNode* folder = space.AddLiveFolder(
      space.pinned_root(), space.pinned_root()->children().size(), u"Feed",
      LiveSource("compass.rss",
                 base::DictValue().Set("url", "https://feed.test/rss")));
  std::vector<LiveItem> items;
  for (const char* key : keys) {
    items.push_back({key, GURL(std::string("https://feed.test/") + key),
                     base::UTF8ToUTF16(key)});
  }
  space.ReplaceLiveItems(folder, items, base::Time::Now(),
                         [](const SidebarNode&) { return false; });
  return folder;
}

TEST_F(CompassSpacesSyncBridgeTest, LiveFoldersSyncWithoutTheirEntries) {
  std::unique_ptr<Device> a = MakeDevice();
  SidebarNode* folder = AddLiveFolderWithEntries(a->space(0), {"1", "2"});
  a->Join();

  // The space and the folder; not its entries.
  EXPECT_EQ(server_.entities.size(), 2u);
  const sync_pb::CompassNode& synced =
      server_.entities[Key(folder->id())].node();
  EXPECT_EQ(synced.type(), kNodeFolder);
  EXPECT_EQ(synced.live_source().plugin_id(), "compass.rss");
  EXPECT_EQ(synced.live_source().config_json(),
            R"({"url":"https://feed.test/rss"})");

  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  SidebarNode* copy = b->space(0).FindById(folder->id());
  ASSERT_TRUE(copy);
  EXPECT_TRUE(copy->is_live_folder());
  EXPECT_EQ(*copy->live_source(), *folder->live_source());
  EXPECT_TRUE(copy->children().empty());

  // B makes its own entries: nothing to send.
  b->space(0).ReplaceLiveItems(
      copy, {{"1", GURL("https://feed.test/1"), u"1"}}, base::Time::Now(),
      [](const SidebarNode&) { return false; });
  EXPECT_EQ(b->Push(), 0u);

  // A changes the feed: B's folder follows.
  a->space(0).SetLiveSource(
      folder, LiveSource("compass.rss",
                         base::DictValue().Set("url", "https://other.test/")));
  EXPECT_EQ(a->Push(), 1u);
  b->Pull();
  EXPECT_EQ(*copy->live_source()->config.FindString("url"),
            "https://other.test/");
  ASSERT_EQ(copy->children().size(), 1u);

  // An entry dragged out is an ordinary tab, and syncs.
  SidebarNode* entry = folder->children()[0].get();
  a->space(0).Move(entry, a->space(0).pinned_root(), 0);
  EXPECT_EQ(a->Push(), 1u);
  b->Pull();
  EXPECT_TRUE(b->space(0).FindById(entry->id()));
}

TEST_F(CompassSpacesSyncBridgeTest, ConvertedElsewhereKeepsOnlyThoseEntries) {
  std::unique_ptr<Device> a = MakeDevice();
  SidebarNode* folder = AddLiveFolderWithEntries(a->space(0), {"1", "2"});
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  SidebarNode* copy = b->space(0).FindById(folder->id());
  ASSERT_TRUE(copy);
  b->space(0).ReplaceLiveItems(
      copy,
      {{"1", GURL("https://feed.test/1"), u"1"},
       {"3", GURL("https://feed.test/3"), u"3"}},
      base::Time::Now(), [](const SidebarNode&) { return false; });

  // A makes it a normal folder: its two entries sync as tabs.
  a->space(0).ConvertToPlainFolder(folder);
  EXPECT_EQ(a->Push(), 3u);
  b->Pull();
  EXPECT_FALSE(copy->is_live_folder());
  // A's entries, not B's own as well.
  ASSERT_EQ(copy->children().size(), 2u);
  for (const auto& child : copy->children()) {
    EXPECT_FALSE(child->is_live_item());
    EXPECT_TRUE(a->space(0).FindById(child->id()));
  }
}

TEST_F(CompassSpacesSyncBridgeTest, DeletedElsewhereTakesItsEntries) {
  std::unique_ptr<Device> a = MakeDevice();
  SidebarNode* folder = AddLiveFolderWithEntries(a->space(0), {"1", "2"});
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  b->space(0).Remove(b->space(0).FindById(folder->id()));
  EXPECT_EQ(b->Push(), 1u);
  a->Pull();
  // Gone with its entries: none were rescued to Pinned.
  EXPECT_TRUE(a->space(0).pinned_root()->children().empty());
  EXPECT_EQ(a->last_push(), 0u);
}

sync_pb::CompassMemory Memory(const std::string& text, int64_t update_time) {
  sync_pb::CompassMemory memory;
  memory.set_uuid(base::Uuid::GenerateRandomV4().AsLowercaseString());
  memory.set_text(text);
  memory.set_create_time(1);
  memory.set_update_time(update_time);
  return memory;
}

// Claude's memory (docs/design/claude.md, "Memory"): added, edited and
// deleted on one device, the same on the others; a device whose memory
// isn't loaded leaves the account's alone.
TEST_F(CompassSpacesSyncBridgeTest, MemoriesSync) {
  std::unique_ptr<Device> a = MakeDevice();
  const sync_pb::CompassMemory italian = Memory("Prefers Italian", 10);
  const std::string key = italian.uuid();
  a->delegate().memories.emplace();
  (*a->delegate().memories)[key] = italian;
  a->Join();
  ASSERT_TRUE(server_.entities.contains(key));
  EXPECT_EQ(server_.entities[key].memory().text(), "Prefers Italian");

  std::unique_ptr<Device> b = MakeDevice();
  b->delegate().memories.emplace();
  b->Join();
  ASSERT_TRUE(b->delegate().memories->contains(key));
  EXPECT_EQ(b->Push(), 0u) << "no echo";

  sync_pb::CompassMemory edited = italian;
  edited.set_text("Prefers Italian, short answers");
  edited.set_update_time(20);
  (*b->delegate().memories)[key] = edited;
  EXPECT_EQ(b->Push(), 1u);
  a->Pull();
  EXPECT_EQ(a->delegate().memories->at(key).text(),
            "Prefers Italian, short answers");
  EXPECT_EQ(a->Push(), 0u) << "no echo";

  // Not loaded: nothing of the account's goes.
  std::unique_ptr<Device> c = MakeDevice();
  c->Join();
  EXPECT_TRUE(server_.entities.contains(key));

  a->delegate().memories->erase(key);
  a->Push();
  EXPECT_FALSE(server_.entities.contains(key));
  b->Pull();
  EXPECT_FALSE(b->delegate().memories->contains(key));
}

TEST_F(CompassSpacesSyncBridgeTest, MemoryEntitiesAreValid) {
  std::unique_ptr<Device> a = MakeDevice();
  sync_pb::CompassSpacesSpecifics specifics;
  *specifics.mutable_memory() = Memory("Lives in Milan", 1);
  syncer::EntityData data = Device::Data(specifics);
  EXPECT_TRUE(a->bridge().IsEntityDataValid(data));
  EXPECT_EQ(a->bridge().GetStorageKey(data), specifics.memory().uuid());
  data.specifics.mutable_compass_spaces()->mutable_memory()->set_uuid("x");
  EXPECT_FALSE(a->bridge().IsEntityDataValid(data));
}


// Spaces that don't sync (docs/design/sync.md) ------------------------------

// The names of |list|'s spaces, in order.
std::string SpaceNames(const SpaceList& list) {
  std::string result;
  for (const auto& space : list.spaces()) {
    result += (result.empty() ? "" : " ") + base::UTF16ToUTF8(space->name());
  }
  return result;
}

// Whether the server has anything of the space |id|: the space or a node in
// it.
bool ServerHasSpace(const Server& server, const base::Uuid& id) {
  const std::string key = id.AsLowercaseString();
  return std::ranges::any_of(server.entities, [&key](const auto& entry) {
    const sync_pb::CompassSpacesSpecifics& specifics = entry.second;
    return (specifics.has_space() && specifics.space().uuid() == key) ||
           (specifics.has_node() && specifics.node().space_uuid() == key);
  });
}

// Nothing of a space that stays on this device goes up; the favorites do.
TEST_F(CompassSpacesSyncBridgeTest, LocalSpaceIsNotUploaded) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel* local = a->list().AddSpace(1, u"Private");
  local->SetSynced(false);
  local->AddTab(local->pinned_root(), 0, GURL("https://bank.test/"), u"Bank");
  local->AddTab(local->today_root(), 0, GURL("https://doc.test/"), u"Doc");
  a->Join();
  EXPECT_FALSE(ServerHasSpace(server_, local->id()));
  EXPECT_TRUE(ServerHasSpace(server_, a->space(0).id()));

  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  EXPECT_EQ(SpaceNames(b->list()), "Main");
  EXPECT_EQ(Describe(*b->favorites().pinned_root()), "Fav");

  // Changes to it don't go up either.
  local->SetName(u"Secret");
  local->AddTab(local->today_root(), 1, GURL("https://more.test/"), u"More");
  EXPECT_EQ(a->Push(), 0u);
}

// Turning sync off removes the space from the other devices and keeps it
// here; turning it back on uploads it again.
TEST_F(CompassSpacesSyncBridgeTest, TurningSyncOffDeletesRemotely) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel* second = a->list().AddSpace(1, u"Second");
  second->AddTab(second->pinned_root(), 0, GURL("https://s.test/"), u"S");
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  ASSERT_EQ(SpaceNames(b->list()), "Main Second");

  second->SetSynced(false);
  EXPECT_EQ(a->Push(), 2u);  // The space and its tab.
  EXPECT_FALSE(ServerHasSpace(server_, second->id()));
  b->Pull();
  EXPECT_EQ(SpaceNames(b->list()), "Main");
  EXPECT_EQ(SpaceNames(a->list()), "Main Second");
  EXPECT_EQ(Describe(*second->pinned_root()), "S");
}

TEST_F(CompassSpacesSyncBridgeTest, TurningSyncOnUploads) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel* local = a->list().AddSpace(1, u"Second");
  local->SetSynced(false);
  local->AddTab(local->pinned_root(), 0, GURL("https://s.test/"), u"S");
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  ASSERT_EQ(SpaceNames(b->list()), "Main");

  local->SetSynced(true);
  EXPECT_EQ(a->Push(), 2u);
  b->Pull();
  ASSERT_EQ(SpaceNames(b->list()), "Main Second");
  EXPECT_EQ(Describe(*b->space(1).pinned_root()), "S");
  EXPECT_EQ(b->space(1).id(), local->id());
}

// A device that changed the space before it heard it went local: its changes
// don't reach the local space, and the delete wins in the end.
TEST_F(CompassSpacesSyncBridgeTest, LocalSpaceIgnoresRemoteChanges) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  SpaceModel* second = a->list().AddSpace(1, u"Second");
  second->AddTab(second->pinned_root(), 0, GURL("https://s.test/"), u"S");
  second->AddTab(second->pinned_root(), 1, GURL("https://t.test/"), u"T");
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();

  second->SetSynced(false);
  a->Push();
  // B hasn't heard yet: renames it, removes a tab, adds one.
  SpaceModel& second_b = b->space(1);
  second_b.SetName(u"Renamed");
  second_b.Remove(FindByTitle(*second_b.pinned_root(), u"T"));
  second_b.AddTab(second_b.pinned_root(), 1, GURL("https://u.test/"), u"U");
  b->Push();

  a->Pull();
  EXPECT_EQ(second->name(), u"Second");
  EXPECT_EQ(Describe(*second->pinned_root()), "S T");
  // A deleted it all again.
  EXPECT_GT(a->last_push(), 0u);
  EXPECT_FALSE(ServerHasSpace(server_, second->id()));
  b->Pull();
  EXPECT_EQ(SpaceNames(b->list()), "Main");
}

// Remote reorders leave a local space after the synced space it followed.
TEST_F(CompassSpacesSyncBridgeTest, LocalSpacesKeepTheirPlace) {
  std::unique_ptr<Device> a = MakeDevice();
  Populate(*a);
  a->list().AddSpace(1, u"Local")->SetSynced(false);
  a->list().AddSpace(2, u"Second");
  a->Join();
  std::unique_ptr<Device> b = MakeDevice();
  b->Join();
  ASSERT_EQ(SpaceNames(b->list()), "Main Second");

  b->list().MoveSpace(b->space(1), 0);
  b->Push();
  a->Pull();
  EXPECT_EQ(SpaceNames(a->list()), "Second Main Local");
}

}  // namespace
}  // namespace compass::sync
