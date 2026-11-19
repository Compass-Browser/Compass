// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/easels/easel_store.h"

#include <memory>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::easels {
namespace {

class EaselStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    Open();
  }

  void Open() {
    store_.reset();
    task_environment_.RunUntilIdle();
    store_ = std::make_unique<EaselStore>(dir_.GetPath().AppendASCII("E"));
    base::RunLoop loop;
    store_->WhenLoaded(loop.QuitClosure());
    loop.Run();
  }

  // Writes everything and reads it back in a new store.
  void Reopen() {
    store_->Flush();
    Open();
  }

  sync_pb::CompassEaselObject Text(const std::string& easel,
                                   const std::string& text) {
    sync_pb::CompassEaselObject object;
    object.set_easel_uuid(easel);
    object.set_x(10);
    object.set_y(20);
    object.set_width(100);
    object.set_height(40);
    object.mutable_text()->set_text(text);
    return object;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  std::unique_ptr<EaselStore> store_;
};

TEST_F(EaselStoreTest, BoardsAndObjectsPersist) {
  auto easel = store_->CreateEasel("  Trip  ");
  ASSERT_TRUE(easel.has_value());
  EXPECT_EQ("Trip", easel->title());
  auto saved = store_->SaveObject(Text(easel->uuid(), "Pack"));
  ASSERT_TRUE(saved.has_value()) << saved.error();
  EXPECT_FALSE(saved->uuid().empty());
  EXPECT_GT(saved->create_time(), 0);

  Reopen();
  ASSERT_EQ(1u, store_->Easels().size());
  EXPECT_EQ("Trip", store_->Easels()[0]->title());
  auto objects = store_->ObjectsOf(easel->uuid());
  ASSERT_EQ(1u, objects.size());
  EXPECT_EQ("Pack", objects[0]->text().text());
}

TEST_F(EaselStoreTest, ObjectsNeedOneKindOfContentAndABoard) {
  auto easel = store_->CreateEasel("B");
  ASSERT_TRUE(easel.has_value());
  sync_pb::CompassEaselObject none = Text(easel->uuid(), "x");
  none.clear_text();
  EXPECT_FALSE(store_->SaveObject(none).has_value());

  sync_pb::CompassEaselObject two = Text(easel->uuid(), "x");
  two.mutable_shape()->set_kind(kRectangle);
  EXPECT_FALSE(store_->SaveObject(two).has_value());

  EXPECT_FALSE(
      store_->SaveObject(Text("00000000-0000-4000-8000-000000000000", "x"))
          .has_value());

  sync_pb::CompassEaselObject arrow = Text(easel->uuid(), "");
  arrow.clear_text();
  arrow.mutable_shape()->set_kind(kArrow);
  arrow.mutable_shape()->add_points(0);
  arrow.mutable_shape()->add_points(0);
  EXPECT_FALSE(store_->SaveObject(arrow).has_value());
  arrow.mutable_shape()->add_points(5);
  arrow.mutable_shape()->add_points(5);
  EXPECT_TRUE(store_->SaveObject(arrow).has_value());
}

TEST_F(EaselStoreTest, LongStrokesAreSimplified) {
  auto easel = store_->CreateEasel("B");
  sync_pb::CompassEaselObject stroke = Text(easel->uuid(), "");
  stroke.clear_text();
  stroke.mutable_shape()->set_kind(kFreehand);
  for (int i = 0; i < 10000; ++i) {
    stroke.mutable_shape()->add_points(i);
    stroke.mutable_shape()->add_points(-i);
  }
  auto saved = store_->SaveObject(stroke);
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(static_cast<int>(kMaxFreehandPoints * 2),
            saved->shape().points_size());
  // The ends stay.
  EXPECT_EQ(0, saved->shape().points(0));
  EXPECT_EQ(9999, saved->shape().points(saved->shape().points_size() - 2));
}

TEST_F(EaselStoreTest, ImagesAreKeptOnceAndPersist) {
  auto sha = store_->AddAsset("png-bytes", "image/png");
  ASSERT_TRUE(sha.has_value());
  EXPECT_EQ(Sha256Hex("png-bytes"), *sha);
  EXPECT_EQ(*sha, store_->AddAsset("png-bytes", "image/png").value());
  EXPECT_FALSE(store_->AddAsset("x", "image/gif").has_value());
  EXPECT_FALSE(
      store_->AddAsset(std::string(kMaxAssetBytes + 1, 'x'), "image/png")
          .has_value());

  auto easel = store_->CreateEasel("B");
  sync_pb::CompassEaselObject image = Text(easel->uuid(), "");
  image.clear_text();
  image.mutable_image()->set_asset(*sha);
  ASSERT_TRUE(store_->SaveObject(image).has_value());

  task_environment_.RunUntilIdle();
  Reopen();
  const sync_pb::CompassEaselAsset* asset = store_->FindAsset(*sha);
  ASSERT_TRUE(asset);
  EXPECT_EQ("png-bytes", asset->data());
  EXPECT_EQ(AssetUuid(*sha), asset->uuid());
}

TEST_F(EaselStoreTest, AnObjectNeedsItsImages) {
  auto easel = store_->CreateEasel("B");
  sync_pb::CompassEaselObject image = Text(easel->uuid(), "");
  image.clear_text();
  image.mutable_image()->set_asset(Sha256Hex("missing"));
  EXPECT_FALSE(store_->SaveObject(image).has_value());
}

TEST_F(EaselStoreTest, DeletingABoardDeletesItsObjects) {
  auto a = store_->CreateEasel("A");
  auto b = store_->CreateEasel("B");
  ASSERT_TRUE(store_->SaveObject(Text(a->uuid(), "1")).has_value());
  ASSERT_TRUE(store_->SaveObject(Text(b->uuid(), "2")).has_value());
  EXPECT_TRUE(store_->DeleteEasel(a->uuid()));
  EXPECT_TRUE(store_->ObjectsOf(a->uuid()).empty());
  EXPECT_EQ(1u, store_->ObjectsOf(b->uuid()).size());
  EXPECT_FALSE(store_->DeleteEasel(a->uuid()));
}

TEST_F(EaselStoreTest, RemoteChangesTheLaterEditWins) {
  auto easel = store_->CreateEasel("Mine");
  ASSERT_TRUE(easel.has_value());

  EaselStore::Entities older;
  sync_pb::CompassEasel old_copy = *easel;
  old_copy.set_title("Older");
  old_copy.set_update_time(easel->update_time() - 1);
  older.easels.push_back(old_copy);
  store_->ApplyRemote(std::move(older), {});
  EXPECT_EQ("Mine", store_->FindEasel(easel->uuid())->title());

  EaselStore::Entities newer;
  sync_pb::CompassEasel new_copy = *easel;
  new_copy.set_title("Theirs");
  new_copy.set_update_time(easel->update_time() + 1);
  newer.easels.push_back(new_copy);
  store_->ApplyRemote(std::move(newer), {});
  EXPECT_EQ("Theirs", store_->FindEasel(easel->uuid())->title());

  store_->ApplyRemote({}, {easel->uuid()});
  EXPECT_FALSE(store_->FindEasel(easel->uuid()));
}

TEST_F(EaselStoreTest, RemoteImagesMustMatchTheirHash) {
  EaselStore::Entities changed;
  sync_pb::CompassEaselAsset good;
  good.set_sha256(Sha256Hex("abc"));
  good.set_uuid(AssetUuid(good.sha256()));
  good.set_mime_type("image/png");
  good.set_data("abc");
  sync_pb::CompassEaselAsset bad = good;
  bad.set_sha256(Sha256Hex("other"));
  bad.set_uuid(AssetUuid(bad.sha256()));
  changed.assets = {good, bad};
  store_->ApplyRemote(std::move(changed), {});
  EXPECT_TRUE(store_->FindAsset(good.sha256()));
  EXPECT_FALSE(store_->FindAsset(bad.sha256()));
}

TEST_F(EaselStoreTest, UnknownContentFromSyncIsKept) {
  auto easel = store_->CreateEasel("B");
  // A content kind from a newer version: field 99, a length-delimited
  // value, which this version keeps as an unknown field.
  sync_pb::CompassEaselObject object;
  object.set_uuid("11111111-1111-4111-8111-111111111111");
  object.set_easel_uuid(easel->uuid());
  std::string bytes = object.SerializeAsString();
  bytes += std::string("\x9a\x06\x02hi", 5);  // field 99, wire type 2.
  sync_pb::CompassEaselObject future;
  ASSERT_TRUE(future.ParseFromString(bytes));
  EaselStore::Entities changed;
  changed.objects.push_back(future);
  store_->ApplyRemote(std::move(changed), {});
  Reopen();
  const sync_pb::CompassEaselObject* kept =
      store_->FindObject(object.uuid());
  ASSERT_TRUE(kept);
  EXPECT_EQ(bytes, kept->SerializeAsString());
}

TEST_F(EaselStoreTest, Urls) {
  const std::string uuid = "0f8fad5b-d9cb-469f-a165-70867728950e";
  EXPECT_EQ("compass://easel/" + uuid, EaselUrl(uuid));
  EXPECT_EQ(uuid, EaselUuidFromUrl("compass://easel/" + uuid));
  EXPECT_EQ(uuid, EaselUuidFromUrl("chrome://easel/" + uuid + "?x#y"));
  EXPECT_EQ(uuid, EaselUuidFromUrl("COMPASS://EASEL/0F8FAD5B-D9CB-469F-A165-"
                                   "70867728950E"));
  EXPECT_EQ("", EaselUuidFromUrl("compass://easel/"));
  EXPECT_EQ("", EaselUuidFromUrl("https://easel/" + uuid));
}

}  // namespace
}  // namespace compass::easels
