// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/easel/easel_conversions.h"

#include <utility>

#include "base/base64.h"
#include "base/time/time.h"
#include "compass/browser/easels/easel_store.h"

namespace compass::easel {
namespace {

// Absent string fields are "" on the page.
#define SET_OR_CLEAR(message, field, value) \
  do {                                      \
    if ((value).empty()) {                  \
      (message)->clear_##field();           \
    } else {                                \
      (message)->set_##field(value);        \
    }                                       \
  } while (0)

mojom::ObjectKind KindOf(const sync_pb::CompassEaselObject& object) {
  if (object.has_text()) {
    return mojom::ObjectKind::kText;
  }
  if (object.has_shape()) {
    return mojom::ObjectKind::kShape;
  }
  if (object.has_image()) {
    return mojom::ObjectKind::kImage;
  }
  if (object.has_link()) {
    return mojom::ObjectKind::kLink;
  }
  if (object.has_capture()) {
    return mojom::ObjectKind::kCapture;
  }
  return mojom::ObjectKind::kUnknown;
}

bool HasKnownContent(const sync_pb::CompassEaselObject& object) {
  return KindOf(object) != mojom::ObjectKind::kUnknown;
}

// Keeps only the content kind the page is setting, so a duplicate made from
// another kind's base still has exactly one.
void ClearContentExcept(sync_pb::CompassEaselObject& object,
                        mojom::ObjectKind keep) {
  if (keep != mojom::ObjectKind::kText) {
    object.clear_text();
  }
  if (keep != mojom::ObjectKind::kShape) {
    object.clear_shape();
  }
  if (keep != mojom::ObjectKind::kImage) {
    object.clear_image();
  }
  if (keep != mojom::ObjectKind::kLink) {
    object.clear_link();
  }
  if (keep != mojom::ObjectKind::kCapture) {
    object.clear_capture();
  }
}

}  // namespace

double SyncTimeToJsTime(int64_t sync_time) {
  if (!sync_time) {
    return 0;
  }
  return easels::FromSyncTime(sync_time).InMillisecondsFSinceUnixEpoch();
}

mojom::EaselSummaryPtr ToMojo(const sync_pb::CompassEasel& easel,
                              int object_count) {
  auto summary = mojom::EaselSummary::New();
  summary->uuid = easel.uuid();
  summary->title = easel.title();
  summary->palette = easel.palette();
  summary->create_time_ms = SyncTimeToJsTime(easel.create_time());
  summary->update_time_ms = SyncTimeToJsTime(easel.update_time());
  summary->object_count = object_count;
  return summary;
}

mojom::EaselPtr ToMojoEasel(const sync_pb::CompassEasel& easel) {
  auto result = mojom::Easel::New();
  result->uuid = easel.uuid();
  result->title = easel.title();
  result->palette = easel.palette();
  return result;
}

mojom::EaselObjectPtr ToMojo(const sync_pb::CompassEaselObject& object) {
  auto result = mojom::EaselObject::New();
  result->uuid = object.uuid();
  result->x = object.x();
  result->y = object.y();
  result->width = object.width();
  result->height = object.height();
  result->z = object.z();
  result->kind = KindOf(object);
  switch (result->kind) {
    case mojom::ObjectKind::kText: {
      const sync_pb::CompassEaselText& text = object.text();
      result->text = mojom::TextContent::New(text.text(), text.color(),
                                             text.font(), text.style());
      break;
    }
    case mojom::ObjectKind::kShape: {
      const sync_pb::CompassEaselShape& shape = object.shape();
      result->shape = mojom::ShapeContent::New(
          shape.kind(), shape.color(),
          std::vector<double>(shape.points().begin(), shape.points().end()),
          shape.has_stroke_width() ? shape.stroke_width() : 0,
          shape.filled());
      break;
    }
    case mojom::ObjectKind::kImage: {
      const sync_pb::CompassEaselImage& image = object.image();
      result->image = mojom::ImageContent::New(
          image.asset(), image.pixel_width(), image.pixel_height());
      break;
    }
    case mojom::ObjectKind::kLink: {
      const sync_pb::CompassEaselLink& link = object.link();
      result->link = mojom::LinkContent::New(
          link.url(), link.title(), link.description(), link.image_asset());
      break;
    }
    case mojom::ObjectKind::kCapture: {
      const sync_pb::CompassEaselCapture& capture = object.capture();
      result->capture = mojom::CaptureContent::New(
          capture.url(), capture.title(), capture.image_asset(),
          capture.favicon_asset(), SyncTimeToJsTime(capture.capture_time()));
      break;
    }
    case mojom::ObjectKind::kUnknown:
      break;
  }
  result->base = base::Base64Encode(object.SerializeAsString());
  return result;
}

base::expected<sync_pb::CompassEaselObject, std::string> FromMojo(
    const mojom::EaselObject& object,
    std::string_view easel_uuid,
    const sync_pb::CompassEaselObject* existing) {
  sync_pb::CompassEaselObject result;
  if (existing) {
    result = *existing;
  } else if (!object.base.empty()) {
    std::string bytes;
    if (!base::Base64Decode(object.base, &bytes) ||
        !result.ParseFromString(bytes)) {
      return base::unexpected("The object couldn't be read.");
    }
  }
  result.set_uuid(object.uuid);
  result.set_easel_uuid(std::string(easel_uuid));
  result.set_x(object.x);
  result.set_y(object.y);
  result.set_width(object.width);
  result.set_height(object.height);
  result.set_z(object.z);

  switch (object.kind) {
    case mojom::ObjectKind::kText: {
      if (!object.text) {
        return base::unexpected("A text object needs its text.");
      }
      ClearContentExcept(result, object.kind);
      sync_pb::CompassEaselText* text = result.mutable_text();
      text->set_text(object.text->text);
      SET_OR_CLEAR(text, color, object.text->color);
      SET_OR_CLEAR(text, font, object.text->font);
      SET_OR_CLEAR(text, style, object.text->style);
      break;
    }
    case mojom::ObjectKind::kShape: {
      if (!object.shape) {
        return base::unexpected("A shape needs its drawing.");
      }
      ClearContentExcept(result, object.kind);
      sync_pb::CompassEaselShape* shape = result.mutable_shape();
      shape->set_kind(object.shape->kind);
      SET_OR_CLEAR(shape, color, object.shape->color);
      shape->clear_points();
      for (double point : object.shape->points) {
        shape->add_points(point);
      }
      if (object.shape->stroke_width > 0) {
        shape->set_stroke_width(object.shape->stroke_width);
      } else {
        shape->clear_stroke_width();
      }
      if (object.shape->filled) {
        shape->set_filled(true);
      } else {
        shape->clear_filled();
      }
      break;
    }
    case mojom::ObjectKind::kImage: {
      if (!object.image || object.image->asset.empty()) {
        return base::unexpected("An image object needs its image.");
      }
      ClearContentExcept(result, object.kind);
      sync_pb::CompassEaselImage* image = result.mutable_image();
      image->set_asset(object.image->asset);
      image->set_pixel_width(object.image->pixel_width);
      image->set_pixel_height(object.image->pixel_height);
      break;
    }
    case mojom::ObjectKind::kLink: {
      if (!object.link || object.link->url.empty()) {
        return base::unexpected("A link needs its URL.");
      }
      ClearContentExcept(result, object.kind);
      sync_pb::CompassEaselLink* link = result.mutable_link();
      link->set_url(object.link->url);
      SET_OR_CLEAR(link, title, object.link->title);
      SET_OR_CLEAR(link, description, object.link->description);
      SET_OR_CLEAR(link, image_asset, object.link->image_asset);
      break;
    }
    case mojom::ObjectKind::kCapture:
      // Only "Capture to Easel" makes captures; the page moves them, and
      // duplicates them from their base.
      if (!result.has_capture()) {
        return base::unexpected("A capture can't be made here.");
      }
      break;
    case mojom::ObjectKind::kUnknown:
      // Kept as received: the content is a kind this version doesn't know.
      if (HasKnownContent(result) || result.unknown_fields().empty()) {
        return base::unexpected("The object couldn't be read.");
      }
      break;
  }
  return result;
}

#undef SET_OR_CLEAR

}  // namespace compass::easel
