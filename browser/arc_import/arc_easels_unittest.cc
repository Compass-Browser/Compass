// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/arc_easels.h"

#include <bit>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/leveldatabase/env_chromium.h"
#include "third_party/leveldatabase/src/include/leveldb/db.h"

namespace compass::arc {
namespace {

// A tiny protobuf encoder, for Firestore's client protos.
std::string Varint(uint64_t value) {
  std::string out;
  while (value >= 0x80) {
    out.push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
  return out;
}

std::string Tag(int field, int type) {
  return Varint((static_cast<uint64_t>(field) << 3) | type);
}

std::string Bytes(int field, std::string_view bytes) {
  return base::StrCat({Tag(field, 2), Varint(bytes.size()), bytes});
}

std::string VarintField(int field, uint64_t value) {
  return Tag(field, 0) + Varint(value);
}

// google.firestore.v1.Value.
std::string String(std::string_view value) {
  return Bytes(17, value);
}

std::string Double(double value) {
  uint64_t bits = std::bit_cast<uint64_t>(value);
  std::string out = Tag(3, 1);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>(bits & 0xff));
    bits >>= 8;
  }
  return out;
}

std::string Integer(int64_t value) {
  return VarintField(2, static_cast<uint64_t>(value));
}

std::string Bool(bool value) {
  return VarintField(1, value ? 1 : 0);
}

std::string TimestampMessage(int64_t seconds) {
  return VarintField(1, static_cast<uint64_t>(seconds));
}

std::string Timestamp(int64_t seconds) {
  return Bytes(10, TimestampMessage(seconds));
}

using Fields = std::vector<std::pair<std::string, std::string>>;

std::string Entries(int field, const Fields& fields) {
  std::string out;
  for (const auto& [key, value] : fields) {
    out += Bytes(field, Bytes(1, key) + Bytes(2, value));
  }
  return out;
}

std::string Map(const Fields& fields) {
  return Bytes(6, Entries(1, fields));
}

std::string Array(const std::vector<std::string>& values) {
  std::string array;
  for (const std::string& value : values) {
    array += Bytes(1, value);
  }
  return Bytes(9, array);
}

constexpr char kRoot[] =
    "projects/bcny-arc-server/databases/(default)/documents/";

// firestore.client.MaybeDocument with a google.firestore.v1.Document.
std::string Document(std::string_view path,
                     const Fields& fields,
                     int64_t updated) {
  const std::string document =
      Bytes(1, base::StrCat({kRoot, path})) + Entries(2, fields) +
      Bytes(3, TimestampMessage(updated - 10)) +
      Bytes(4, TimestampMessage(updated));
  return Bytes(2, document);
}

// A document known deleted (firestore.client.NoDocument), as of |read|.
std::string NoDocument(std::string_view path, int64_t read) {
  return Bytes(1, Bytes(1, base::StrCat({kRoot, path})) +
                      Bytes(2, TimestampMessage(read)));
}

// A "remote_document" key; the parts after the table name aren't read.
std::string Key(std::string_view path, std::string_view suffix = "") {
  return base::StrCat({std::string_view("\x85remote_document\x00\x01", 18),
                       "\xbe", path, std::string_view("\x00\x01", 2), suffix,
                       "\x80"});
}

std::string Rect(double x, double y, double width, double height, double z) {
  return Map({{"rect", Map({{"x", Double(x)},
                            {"y", Double(y)},
                            {"width", Double(width)},
                            {"height", Double(height)}})},
              {"zIndex", Double(z)}});
}

Fields ObjectFields(std::string_view id,
                    std::string position,
                    std::string content) {
  return {{"id", Map({{"value", String(id)}})},
          {"position", std::move(position)},
          {"content", std::move(content)},
          {"currentWorkingVersion", String("V")}};
}

std::string ImageValue(std::string_view url, double width, double height) {
  return Map({{"content", Map({{"url", String(url)}})},
              {"id", Map({{"value", String("I")}})},
              {"dominantColors", Array({})},
              {"size", Array({Double(width), Double(height)})}});
}

constexpr char kImage[] =
    "https://firebasestorage.googleapis.com:443/v0/b/bucket/o/"
    "easelImages%2Fa.png?alt=media&token=t";

class ArcEaselsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    leveldb_env::Options options;
    options.create_if_missing = true;
    ASSERT_TRUE(
        leveldb_env::OpenDB(options, dir_.GetPath().AsUTF8Unsafe(), &db_)
            .ok());
  }

  void Put(const std::string& key, const std::string& value) {
    ASSERT_TRUE(db_->Put(leveldb::WriteOptions(), key, value).ok());
  }

  // Writes a board with one object of each kind, its title object, a
  // deleted object and one cached twice.
  void WriteBoard() {
    Put(Key("easels/E1"),
        Document("easels/E1",
                 {{"title", String("Plans")},
                  {"palette", String("vibrant")},
                  {"titleObjectID", String("T")},
                  {"lastModified", Timestamp(1700000000)},
                  {"version", Integer(7)},
                  {"shareStatus", String("creatorOnly")}},
                 1700000000));
    Put(Key("easels/E1/objects/T"),
        Document("easels/E1/objects/T",
                 ObjectFields("T", Rect(0.2, 0.035, 0.6, 0.028, 1),
                              Map({{"text", Map({{"string", String("Plans")},
                                                 {"style", Map({})}})}})),
                 1700000000));
    Put(Key("easels/E1/objects/text"),
        Document(
            "easels/E1/objects/text",
            ObjectFields(
                "text", Rect(0.1, 0.2, 0.3, 0.018, 4),
                Map({{"text",
                      Map({{"string", String("Hello")},
                           {"style",
                            Map({{"fillStyle", String("none")},
                                 {"fontFamily", String("clean")},
                                 {"paragraphStyle", String("h2")}})}})}})),
            1700000000));
    Put(Key("easels/E1/objects/rect"),
        Document("easels/E1/objects/rect",
                 ObjectFields(
                     "rect", Rect(0.1, 0.1, 0.2, 0.1, 3.7),
                     Map({{"shape",
                           Map({{"color", String("lightGreen")},
                                {"type",
                                 Map({{"isRectangle", Bool(true)}})}})}})),
                 1700000000));
    Put(Key("easels/E1/objects/circle"),
        Document("easels/E1/objects/circle",
                 ObjectFields(
                     "circle", Rect(0, 0, 0.5, 0.5, 2),
                     Map({{"shape",
                           Map({{"color", String("red")},
                                {"type", Map({{"isCircle", Bool(true)}})}})}})),
                 1700000000));
    Put(Key("easels/E1/objects/arrow"),
        Document(
            "easels/E1/objects/arrow",
            ObjectFields(
                "arrow", Rect(0.4, 0.1, 0.1, 0.2, 5),
                Map({{"shape",
                      Map({{"color", String("black")},
                           {"type",
                            Map({{"arrow",
                                  Map({{"start",
                                        Array({Double(1), Double(0)})},
                                       {"end", Array({Double(0),
                                                      Double(1)})}})}})}})}})),
            1700000000));
    Put(Key("easels/E1/objects/pen"),
        Document(
            "easels/E1/objects/pen",
            ObjectFields(
                "pen", Rect(0.1, 0.1, 0.2, 0.1, 6),
                Map({{"shape",
                      Map({{"color", String("blue")},
                           {"type",
                            Map({{"freehand",
                                  Array({Map({{"x", Double(0)},
                                              {"y", Double(0)}}),
                                         Map({{"x", Double(0.5)},
                                              {"y", Double(1)}}),
                                         Map({{"x", Double(1)},
                                              {"y", Double(0.5)}})})}})}})}})),
            1700000000));
    Put(Key("easels/E1/objects/image"),
        Document("easels/E1/objects/image",
                 ObjectFields("image", Rect(0.4, 0.3, 0.2, 0.1, 7),
                              Map({{"image", ImageValue(kImage, 1920, 1069)}})),
                 1700000000));
    Put(Key("easels/E1/objects/link"),
        Document(
            "easels/E1/objects/link",
            ObjectFields(
                "link", Rect(0.1, 0.3, 0.2, 0.08, 8),
                Map({{"link",
                      Map({{"source", String("https://example.test/a")},
                           {"title", String("Example")},
                           {"description", String("A page")},
                           {"image", ImageValue(kImage, 600, 300)}})}})),
            1700000000));
    Put(Key("easels/E1/objects/card"),
        Document(
            "easels/E1/objects/card",
            ObjectFields(
                "card", Rect(0.4, 0.5, 0.4, 0.1, 9),
                Map({{"webCard",
                      Map({{"url", String("https://example.test/b")},
                           {"text", String("Page B")},
                           {"captureType", Map({{"partialPage", Map({})}})},
                           {"captureLayoutInfo",
                            Map({{"frameRelativeToViewport",
                                  Map({{"x", Double(415.5)},
                                       {"y", Double(209.5)},
                                       {"width", Double(855)},
                                       {"height", Double(217)}})},
                                 {"webContentOffset",
                                  Array({Double(0), Double(120)})},
                                 {"webContentSize",
                                  Array({Double(1434), Double(946)})}})},
                           {"dateCaptured", Timestamp(1683374931)},
                           {"image", ImageValue(kImage, 2048, 521)},
                           {"favicon",
                            Map({{"content",
                                  Map({{"url", String(kImage)}})}})}})}})),
            1700000000));
    // Cached twice: an older version under another key (Firestore's newer
    // key format has the read time in it), the newer one here.
    Put(Key("easels/E1/objects/twice", "\x90\x01"),
        Document("easels/E1/objects/twice",
                 ObjectFields("twice", Rect(0.9, 0.9, 0.05, 0.05, 10),
                              Map({{"text",
                                    Map({{"string", String("New")}})}})),
                 1700000500));
    Put(Key("easels/E1/objects/twice", "\x90\x02"),
        Document("easels/E1/objects/twice",
                 ObjectFields("twice", Rect(0.9, 0.9, 0.05, 0.05, 10),
                              Map({{"text",
                                    Map({{"string", String("Old")}})}})),
                 1700000100));
    // Deleted: removed from the database, or known deleted.
    Put(Key("easels/E1/objects/gone"),
        Document("easels/E1/objects/gone",
                 ObjectFields("gone", Rect(0, 0, 0.1, 0.1, 1),
                              Map({{"text", Map({{"string", String("x")}})}})),
                 1700000000));
    ASSERT_TRUE(
        db_->Delete(leveldb::WriteOptions(), Key("easels/E1/objects/gone"))
            .ok());
    Put(Key("easels/E1/objects/nodoc"),
        NoDocument("easels/E1/objects/nodoc", 1700000000));
    // Deleted after it was cached under another key.
    Put(Key("easels/E1/objects/later", "\x90\x01"),
        Document("easels/E1/objects/later",
                 ObjectFields("later", Rect(0, 0, 0.1, 0.1, 1),
                              Map({{"text", Map({{"string", String("x")}})}})),
                 1700000000));
    Put(Key("easels/E1/objects/later", "\x90\x02"),
        NoDocument("easels/E1/objects/later", 1700000200));
    // A member of the board, and another table: not objects.
    Put(Key("easels/E1/memberships/M"),
        Document("easels/E1/memberships/M", {{"role", String("owner")}},
                 1700000000));
    Put(std::string("\x85remote_document_read_time\x00\x01", 28) + "x",
        Document("easels/E1/objects/other", {}, 1700000000));
    // A board listed but never opened: no objects.
    Put(Key("easels/E2"),
        Document("easels/E2", {{"title", String("Later")}}, 1700000000));
  }

  void Close() { db_.reset(); }

  ArcEasels Read() {
    Close();
    return ReadArcEasels(dir_.GetPath());
  }

  const ArcEaselObject* Find(const ArcEasel& easel, std::string_view id) {
    for (const ArcEaselObject& object : easel.objects) {
      if (object.id == id) {
        return &object;
      }
    }
    return nullptr;
  }

  // LevelDB compacts on the thread pool.
  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  std::unique_ptr<leveldb::DB> db_;
};

TEST_F(ArcEaselsTest, ReadsBoardsAndEveryKindOfObject) {
  WriteBoard();
  ArcEasels easels = Read();
  ASSERT_EQ(easels.size(), 2u);
  const ArcEasel& easel = easels["E1"];
  EXPECT_EQ(easel.id, "E1");
  EXPECT_EQ(easel.title, "Plans");
  EXPECT_EQ(easel.palette, "paper");
  EXPECT_EQ(easel.last_modified,
            base::Time::FromSecondsSinceUnixEpoch(1700000000));
  EXPECT_TRUE(easel.has_objects);
  // The title object, the deleted ones and the member are left out.
  ASSERT_EQ(easel.objects.size(), 9u);
  EXPECT_FALSE(Find(easel, "T"));
  EXPECT_FALSE(Find(easel, "gone"));
  EXPECT_FALSE(Find(easel, "nodoc"));
  EXPECT_FALSE(Find(easel, "later"));
  EXPECT_FALSE(Find(easel, "M"));
  EXPECT_FALSE(Find(easel, "other"));
  // Lowest z first.
  for (size_t i = 1; i < easel.objects.size(); ++i) {
    EXPECT_LE(easel.objects[i - 1].object.z(), easel.objects[i].object.z());
  }

  const ArcEaselObject* text = Find(easel, "text");
  ASSERT_TRUE(text);
  // Boxes are fractions of the board's width.
  EXPECT_DOUBLE_EQ(text->object.x(), 100);
  EXPECT_DOUBLE_EQ(text->object.y(), 200);
  EXPECT_DOUBLE_EQ(text->object.width(), 300);
  EXPECT_DOUBLE_EQ(text->object.height(), 18);
  EXPECT_DOUBLE_EQ(text->object.z(), 4);
  EXPECT_EQ(text->object.text().text(), "Hello");
  EXPECT_EQ(text->object.text().font(), "sans");
  EXPECT_EQ(text->object.text().style(), "heading");
  EXPECT_FALSE(text->object.text().has_color());

  const ArcEaselObject* rect = Find(easel, "rect");
  ASSERT_TRUE(rect);
  EXPECT_EQ(rect->object.shape().kind(), 1);
  EXPECT_TRUE(rect->object.shape().filled());
  EXPECT_FALSE(rect->object.shape().color().empty());

  const ArcEaselObject* circle = Find(easel, "circle");
  ASSERT_TRUE(circle);
  EXPECT_EQ(circle->object.shape().kind(), 2);
  EXPECT_FALSE(circle->object.shape().filled());
  EXPECT_FALSE(circle->object.shape().color().empty());

  // Points are fractions of the box; black is the board's ink.
  const ArcEaselObject* arrow = Find(easel, "arrow");
  ASSERT_TRUE(arrow);
  EXPECT_EQ(arrow->object.shape().kind(), 3);
  EXPECT_FALSE(arrow->object.shape().has_color());
  ASSERT_EQ(arrow->object.shape().points_size(), 4);
  EXPECT_DOUBLE_EQ(arrow->object.shape().points(0), 100);
  EXPECT_DOUBLE_EQ(arrow->object.shape().points(1), 0);
  EXPECT_DOUBLE_EQ(arrow->object.shape().points(2), 0);
  EXPECT_DOUBLE_EQ(arrow->object.shape().points(3), 200);

  const ArcEaselObject* pen = Find(easel, "pen");
  ASSERT_TRUE(pen);
  EXPECT_EQ(pen->object.shape().kind(), 4);
  ASSERT_EQ(pen->object.shape().points_size(), 6);
  EXPECT_DOUBLE_EQ(pen->object.shape().points(2), 100);
  EXPECT_DOUBLE_EQ(pen->object.shape().points(3), 100);
  EXPECT_DOUBLE_EQ(pen->object.shape().points(4), 200);
  EXPECT_DOUBLE_EQ(pen->object.shape().points(5), 50);

  const ArcEaselObject* image = Find(easel, "image");
  ASSERT_TRUE(image);
  ASSERT_TRUE(image->object.has_image());
  EXPECT_EQ(image->object.image().pixel_width(), 1920);
  EXPECT_EQ(image->object.image().pixel_height(), 1069);
  EXPECT_EQ(image->image_url, GURL(kImage));
  EXPECT_TRUE(IsArcEaselImageUrl(image->image_url));

  const ArcEaselObject* link = Find(easel, "link");
  ASSERT_TRUE(link);
  EXPECT_EQ(link->object.link().url(), "https://example.test/a");
  EXPECT_EQ(link->object.link().title(), "Example");
  EXPECT_EQ(link->object.link().description(), "A page");
  EXPECT_EQ(link->image_url, GURL(kImage));

  const ArcEaselObject* card = Find(easel, "card");
  ASSERT_TRUE(card);
  ASSERT_TRUE(card->object.has_capture());
  const sync_pb::CompassEaselCapture& capture = card->object.capture();
  EXPECT_EQ(capture.url(), "https://example.test/b");
  EXPECT_EQ(capture.title(), "Page B");
  EXPECT_DOUBLE_EQ(capture.region_x(), 415.5);
  EXPECT_DOUBLE_EQ(capture.region_y(), 209.5);
  EXPECT_DOUBLE_EQ(capture.region_width(), 855);
  EXPECT_DOUBLE_EQ(capture.region_height(), 217);
  EXPECT_DOUBLE_EQ(capture.scroll_x(), 0);
  EXPECT_DOUBLE_EQ(capture.scroll_y(), 120);
  EXPECT_DOUBLE_EQ(capture.viewport_width(), 1434);
  EXPECT_DOUBLE_EQ(capture.viewport_height(), 946);
  EXPECT_EQ(capture.capture_time(),
            base::Time::FromSecondsSinceUnixEpoch(1683374931)
                .ToDeltaSinceWindowsEpoch()
                .InMicroseconds());
  EXPECT_EQ(card->image_url, GURL(kImage));
  EXPECT_EQ(card->favicon_url, GURL(kImage));

  // The latest version of a document cached twice.
  const ArcEaselObject* twice = Find(easel, "twice");
  ASSERT_TRUE(twice);
  EXPECT_EQ(twice->object.text().text(), "New");

  // Listed, never opened.
  const ArcEasel& later = easels["E2"];
  EXPECT_EQ(later.title, "Later");
  EXPECT_FALSE(later.has_objects);
  EXPECT_TRUE(later.objects.empty());
}

// Arc's cache may be all in its log (.log), in tables (.ldb), or both.
TEST_F(ArcEaselsTest, ReadsTablesAndLogs) {
  WriteBoard();
  db_->CompactRange(nullptr, nullptr);
  // An update after the compaction, in the log only.
  Put(Key("easels/E1"),
      Document("easels/E1",
               {{"title", String("Plans 2")}, {"titleObjectID", String("T")}},
               1700001000));
  ArcEasels easels = Read();
  EXPECT_EQ(easels["E1"].title, "Plans 2");
  EXPECT_EQ(easels["E1"].objects.size(), 9u);
}

// Without CURRENT and the manifest, the copy is rebuilt from what's there.
TEST_F(ArcEaselsTest, RepairsACacheWithoutItsManifest) {
  WriteBoard();
  Close();
  base::DeleteFile(dir_.GetPath().AppendASCII("CURRENT"));
  ArcEasels easels = ReadArcEasels(dir_.GetPath());
  EXPECT_EQ(easels["E1"].objects.size(), 9u);
  // Arc's own folder is left as it was.
  EXPECT_FALSE(base::PathExists(dir_.GetPath().AppendASCII("CURRENT")));
}

TEST_F(ArcEaselsTest, NoCache) {
  Close();
  EXPECT_TRUE(ReadArcEasels(dir_.GetPath().AppendASCII("missing")).empty());
  EXPECT_TRUE(ReadArcEasels(base::FilePath()).empty());
}

TEST_F(ArcEaselsTest, ObjectsWithoutTheirImages) {
  WriteBoard();
  ArcEasels easels = Read();
  const ArcEasel& easel = easels["E1"];

  sync_pb::CompassEaselObject image = WithoutImages(*Find(easel, "image"));
  EXPECT_FALSE(image.has_image());
  EXPECT_EQ(image.link().url(), GURL(kImage).spec());

  sync_pb::CompassEaselObject card = WithoutImages(*Find(easel, "card"));
  EXPECT_FALSE(card.has_capture());
  EXPECT_EQ(card.link().url(), "https://example.test/b");
  EXPECT_EQ(card.link().title(), "Page B");

  ArcEaselObject link = *Find(easel, "link");
  link.object.mutable_link()->set_image_asset("abc");
  EXPECT_FALSE(WithoutImages(link).link().has_image_asset());
  EXPECT_EQ(WithoutImages(link).link().url(), "https://example.test/a");

  // Same box.
  EXPECT_EQ(card.x(), Find(easel, "card")->object.x());
}

TEST(ArcEaselImageUrlTest, OnlyFirebaseStorageOverHttps) {
  EXPECT_TRUE(IsArcEaselImageUrl(GURL(kImage)));
  EXPECT_FALSE(IsArcEaselImageUrl(
      GURL("http://firebasestorage.googleapis.com/v0/b/x/o/y")));
  EXPECT_FALSE(IsArcEaselImageUrl(
      GURL("https://firebasestorage.googleapis.com:8443/v0/b/x/o/y")));
  EXPECT_FALSE(IsArcEaselImageUrl(GURL("https://example.test/a.png")));
  EXPECT_FALSE(IsArcEaselImageUrl(
      GURL("https://firebasestorage.googleapis.com.example.test/a.png")));
  EXPECT_FALSE(IsArcEaselImageUrl(GURL()));
}

TEST(ArcEaselsRefTest, EaselsInSpacesOnce) {
  Item easel;
  easel.kind = Item::Kind::kEasel;
  easel.id = "i1";
  easel.easel_id = "E1";
  easel.saved_title = u"Plans";
  Item again = easel;
  again.id = "i2";
  Item folder;
  folder.kind = Item::Kind::kFolder;
  Item other = easel;
  other.id = "i3";
  other.easel_id = "E2";
  folder.children.push_back(other);
  Space space;
  space.pinned = {easel, folder};
  space.today = {again};
  std::vector<ArcEaselRef> refs = EaselsIn({space});
  ASSERT_EQ(refs.size(), 2u);
  EXPECT_EQ(refs[0].id, "E1");
  EXPECT_EQ(refs[0].title, u"Plans");
  EXPECT_EQ(refs[1].id, "E2");
}

// Reads this computer's Arc Easels cache and logs what's in it (counts and
// kinds, no content). Opt-in:
//   compass_unittests --gtest_also_run_disabled_tests \
//       --gtest_filter=ArcEaselsRealTest.DISABLED_RealArcEasels
// ARC_FIRESTORE may point at another copy of the cache folder.
TEST(ArcEaselsRealTest, DISABLED_RealArcEasels) {
  base::test::TaskEnvironment task_environment;
  base::FilePath dir;
  if (std::optional<std::string> path =
          base::Environment::Create()->GetVar("ARC_FIRESTORE")) {
    dir = base::FilePath::FromUTF8Unsafe(*path);
  } else {
#if BUILDFLAG(IS_WIN)
    // Where Arc for Windows keeps the cache isn't known: give ARC_FIRESTORE.
    GTEST_SKIP() << "set ARC_FIRESTORE to Arc's firestore cache folder";
#else
    base::FilePath app_support;
    ASSERT_TRUE(base::PathService::Get(base::DIR_APP_DATA, &app_support));
    dir = app_support.AppendASCII("firestore")
              .AppendASCII("Arc")
              .AppendASCII("bcny-arc-server")
              .AppendASCII("main");
#endif
  }
  ASSERT_TRUE(base::DirectoryExists(dir));
  ArcEasels easels = ReadArcEasels(dir);
  for (const auto& [id, easel] : easels) {
    std::map<std::string, int> kinds;
    int images = 0;
    for (const ArcEaselObject& object : easel.objects) {
      const sync_pb::CompassEaselObject& o = object.object;
      kinds[o.has_text()      ? "text"
            : o.has_shape()   ? "shape"
            : o.has_image()   ? "image"
            : o.has_link()    ? "link"
            : o.has_capture() ? "capture"
                              : "other"]++;
      images += IsArcEaselImageUrl(object.image_url) ? 1 : 0;
    }
    std::string summary;
    for (const auto& [kind, count] : kinds) {
      summary += base::StrCat({kind, "=", base::NumberToString(count), " "});
    }
    LOG(INFO) << "easel palette=" << easel.palette
              << " has_objects=" << easel.has_objects
              << " objects=" << easel.objects.size() << " " << summary
              << "downloadable_images=" << images;
  }
  LOG(INFO) << "easels=" << easels.size();
}

}  // namespace
}  // namespace compass::arc
