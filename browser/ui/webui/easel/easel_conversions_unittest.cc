// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/easel/easel_conversions.h"

#include <string>

#include "base/time/time.h"
#include "compass/browser/easels/easel_store.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::easel {
namespace {

using ::testing::ElementsAre;

constexpr char kEasel[] = "6f1d3c1e-8c1b-4b9e-9a51-6c1f0d6c2a10";
constexpr char kObject[] = "0b0c9a5e-1d2f-4c3b-8e7a-9f6d5c4b3a21";
constexpr char kCopy[] = "2a3b4c5d-6e7f-4a8b-9c0d-1e2f3a4b5c6d";

// Field 99 as a varint, 1: what a newer client might have added.
constexpr char kUnknownField[] = "\x98\x06\x01";

sync_pb::CompassEaselObject Box() {
  sync_pb::CompassEaselObject object;
  object.set_uuid(kObject);
  object.set_easel_uuid(kEasel);
  object.set_x(10);
  object.set_y(20);
  object.set_width(300);
  object.set_height(80);
  object.set_z(3);
  return object;
}

sync_pb::CompassEaselObject Text() {
  sync_pb::CompassEaselObject object = Box();
  object.mutable_text()->set_text("Hello");
  object.mutable_text()->set_font("serif");
  return object;
}

sync_pb::CompassEaselObject Capture() {
  sync_pb::CompassEaselObject object = Box();
  sync_pb::CompassEaselCapture* capture = object.mutable_capture();
  capture->set_url("https://example.com/");
  capture->set_title("Example");
  capture->set_image_asset("abc");
  capture->set_region_x(5);
  capture->set_scroll_y(400);
  capture->set_capture_time(
      easels::ToSyncTime(base::Time::FromMillisecondsSinceUnixEpoch(1000)));
  return object;
}

TEST(EaselConversionsTest, TextToMojo) {
  mojom::EaselObjectPtr object = ToMojo(Text());
  EXPECT_EQ(object->uuid, kObject);
  EXPECT_EQ(object->kind, mojom::ObjectKind::kText);
  EXPECT_EQ(object->x, 10);
  EXPECT_EQ(object->height, 80);
  EXPECT_EQ(object->z, 3);
  ASSERT_TRUE(object->text);
  EXPECT_EQ(object->text->text, "Hello");
  EXPECT_EQ(object->text->font, "serif");
  EXPECT_EQ(object->text->color, "");
  EXPECT_FALSE(object->shape);
  EXPECT_FALSE(object->base.empty());
}

TEST(EaselConversionsTest, CaptureToMojo) {
  mojom::EaselObjectPtr object = ToMojo(Capture());
  EXPECT_EQ(object->kind, mojom::ObjectKind::kCapture);
  ASSERT_TRUE(object->capture);
  EXPECT_EQ(object->capture->url, "https://example.com/");
  EXPECT_EQ(object->capture->image_asset, "abc");
  EXPECT_DOUBLE_EQ(object->capture->capture_time_ms, 1000);
}

TEST(EaselConversionsTest, EditKeepsWhatThePageDoesntSee) {
  sync_pb::CompassEaselObject stored = Text();
  stored.mutable_unknown_fields()->append(kUnknownField, 3);
  stored.mutable_text()->mutable_unknown_fields()->append(kUnknownField, 3);

  mojom::EaselObjectPtr page = ToMojo(stored);
  page->x = 50;
  page->text->text = "Bye";
  page->text->color = "#ff0000";
  base::expected<sync_pb::CompassEaselObject, std::string> saved =
      FromMojo(*page, kEasel, &stored);
  ASSERT_TRUE(saved.has_value()) << saved.error();
  EXPECT_EQ(saved->x(), 50);
  EXPECT_EQ(saved->y(), 20);
  EXPECT_EQ(saved->text().text(), "Bye");
  EXPECT_EQ(saved->text().color(), "#ff0000");
  EXPECT_EQ(saved->text().font(), "serif");
  EXPECT_EQ(saved->unknown_fields(), std::string(kUnknownField, 3));
  EXPECT_EQ(saved->text().unknown_fields(), std::string(kUnknownField, 3));

  // "" is absent: the board's ink again.
  page->text->color = "";
  saved = FromMojo(*page, kEasel, &stored);
  ASSERT_TRUE(saved.has_value());
  EXPECT_FALSE(saved->text().has_color());
}

TEST(EaselConversionsTest, NewShape) {
  auto page = mojom::EaselObject::New();
  page->uuid = kObject;
  page->width = 100;
  page->height = 50;
  page->kind = mojom::ObjectKind::kShape;
  page->shape = mojom::ShapeContent::New(easels::kArrow, "",
                                         std::vector<double>{0, 0, 100, 50},
                                         /*stroke_width=*/0, /*filled=*/false);
  base::expected<sync_pb::CompassEaselObject, std::string> saved =
      FromMojo(*page, kEasel, nullptr);
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->easel_uuid(), kEasel);
  EXPECT_EQ(saved->shape().kind(), easels::kArrow);
  EXPECT_THAT(saved->shape().points(), ElementsAre(0, 0, 100, 50));
  EXPECT_FALSE(saved->shape().has_stroke_width());
  EXPECT_FALSE(saved->shape().has_color());
  EXPECT_FALSE(saved->shape().has_filled());

  mojom::EaselObjectPtr back = ToMojo(*saved);
  EXPECT_EQ(back->kind, mojom::ObjectKind::kShape);
  EXPECT_EQ(back->shape->stroke_width, 0);
  EXPECT_THAT(back->shape->points, ElementsAre(0, 0, 100, 50));
}

TEST(EaselConversionsTest, DuplicateCaptureFromBase) {
  mojom::EaselObjectPtr page = ToMojo(Capture());
  page->uuid = kCopy;
  page->x = 40;
  base::expected<sync_pb::CompassEaselObject, std::string> saved =
      FromMojo(*page, kEasel, nullptr);
  ASSERT_TRUE(saved.has_value()) << saved.error();
  EXPECT_EQ(saved->uuid(), kCopy);
  EXPECT_EQ(saved->x(), 40);
  EXPECT_EQ(saved->capture().region_x(), 5);
  EXPECT_EQ(saved->capture().scroll_y(), 400);
}

TEST(EaselConversionsTest, PageCantMakeCaptures) {
  mojom::EaselObjectPtr page = ToMojo(Capture());
  page->base.clear();
  EXPECT_FALSE(FromMojo(*page, kEasel, nullptr).has_value());

  page->base = "not base64!";
  EXPECT_FALSE(FromMojo(*page, kEasel, nullptr).has_value());
}

TEST(EaselConversionsTest, UnknownKindMovesAndKeepsItsContent) {
  sync_pb::CompassEaselObject stored = Box();
  // Field 15 (a content kind of a newer client), length 2.
  stored.mutable_unknown_fields()->append("\x7a\x02\x08\x01", 4);

  mojom::EaselObjectPtr page = ToMojo(stored);
  EXPECT_EQ(page->kind, mojom::ObjectKind::kUnknown);
  page->y = 99;
  base::expected<sync_pb::CompassEaselObject, std::string> saved =
      FromMojo(*page, kEasel, &stored);
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->y(), 99);
  EXPECT_FALSE(saved->has_text());
  EXPECT_EQ(saved->unknown_fields(), std::string("\x7a\x02\x08\x01", 4));

  // From its base (undoing its delete) it comes back as it was...
  saved = FromMojo(*page, kEasel, nullptr);
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->unknown_fields(), std::string("\x7a\x02\x08\x01", 4));
  // ...and without one there's nothing to keep.
  page->base.clear();
  EXPECT_FALSE(FromMojo(*page, kEasel, nullptr).has_value());
}

TEST(EaselConversionsTest, ChangingKindLeavesOneContent) {
  mojom::EaselObjectPtr page = ToMojo(Text());
  page->uuid = kCopy;
  page->kind = mojom::ObjectKind::kLink;
  page->text = nullptr;
  page->link = mojom::LinkContent::New("https://example.com/", "Example", "",
                                       "");
  base::expected<sync_pb::CompassEaselObject, std::string> saved =
      FromMojo(*page, kEasel, nullptr);
  ASSERT_TRUE(saved.has_value());
  EXPECT_FALSE(saved->has_text());
  EXPECT_EQ(saved->link().title(), "Example");
  EXPECT_FALSE(saved->link().has_description());
}

TEST(EaselConversionsTest, Summary) {
  sync_pb::CompassEasel easel;
  easel.set_uuid(kEasel);
  easel.set_title("Ideas");
  easel.set_palette("grid");
  easel.set_create_time(
      easels::ToSyncTime(base::Time::FromMillisecondsSinceUnixEpoch(5000)));
  mojom::EaselSummaryPtr summary = ToMojo(easel, 7);
  EXPECT_EQ(summary->title, "Ideas");
  EXPECT_EQ(summary->palette, "grid");
  EXPECT_EQ(summary->object_count, 7);
  EXPECT_DOUBLE_EQ(summary->create_time_ms, 5000);
  EXPECT_EQ(summary->update_time_ms, 0);
}

}  // namespace
}  // namespace compass::easel
