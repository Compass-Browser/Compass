// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/arc_easels.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "base/containers/fixed_flat_map.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "third_party/leveldatabase/env_chromium.h"
#include "third_party/leveldatabase/src/include/leveldb/db.h"
#include "third_party/leveldatabase/src/include/leveldb/iterator.h"
#include "url/url_constants.h"

namespace compass::arc {

ArcEaselObject::ArcEaselObject() = default;
ArcEaselObject::ArcEaselObject(const ArcEaselObject&) = default;
ArcEaselObject::ArcEaselObject(ArcEaselObject&&) = default;
ArcEaselObject& ArcEaselObject::operator=(const ArcEaselObject&) = default;
ArcEaselObject& ArcEaselObject::operator=(ArcEaselObject&&) = default;
ArcEaselObject::~ArcEaselObject() = default;

ArcEasel::ArcEasel() = default;
ArcEasel::ArcEasel(const ArcEasel&) = default;
ArcEasel::ArcEasel(ArcEasel&&) = default;
ArcEasel& ArcEasel::operator=(const ArcEasel&) = default;
ArcEasel& ArcEasel::operator=(ArcEasel&&) = default;
ArcEasel::~ArcEasel() = default;

namespace {

// Firestore's LevelDB keys are OrderedCode: the table's name (label 5, a
// string ending in 00 01), then the key's parts. Cached server documents are
// in "remote_document" (not "remote_document_read_time", an index, nor the
// mutation tables: Arc's own edits not yet sent, which Arc had sent by the
// time it quit if it was online).
constexpr std::string_view kRemoteDocumentPrefix(
    "\x85"
    "remote_document\x00\x01",
    18);

constexpr std::string_view kImageHost = "firebasestorage.googleapis.com";

// Firestore values nest (maps in arrays in maps); deeper is garbage.
constexpr int kMaxDepth = 32;
// More is garbage too; the store simplifies a stroke to 4,000 points.
constexpr size_t kMaxRawPoints = 100000;

// Protobuf's wire format, just what Firestore's documents need.
class WireReader {
 public:
  explicit WireReader(std::string_view data) : data_(data) {}

  // The next field; false at the end, or for malformed data.
  bool Next() {
    if (pos_ >= data_.size()) {
      return false;
    }
    uint64_t key = 0;
    if (!ReadVarint(key)) {
      return Fail();
    }
    field_ = static_cast<int>(key >> 3);
    type_ = static_cast<int>(key & 7);
    switch (type_) {
      case 0:
        if (!ReadVarint(varint_)) {
          return Fail();
        }
        return true;
      case 1:
        if (data_.size() - pos_ < 8) {
          return Fail();
        }
        fixed64_ = 0;
        for (int i = 7; i >= 0; --i) {
          fixed64_ = (fixed64_ << 8) | static_cast<uint8_t>(data_[pos_ + i]);
        }
        pos_ += 8;
        return true;
      case 2: {
        uint64_t length = 0;
        if (!ReadVarint(length) || length > data_.size() - pos_) {
          return Fail();
        }
        bytes_ = data_.substr(pos_, length);
        pos_ += length;
        return true;
      }
      case 5:
        if (data_.size() - pos_ < 4) {
          return Fail();
        }
        pos_ += 4;
        return true;
      default:
        return Fail();
    }
  }

  int field() const { return field_; }
  bool is_varint() const { return type_ == 0; }
  bool is_fixed64() const { return type_ == 1; }
  bool is_bytes() const { return type_ == 2; }
  uint64_t varint() const { return varint_; }
  double fixed64_as_double() const { return std::bit_cast<double>(fixed64_); }
  std::string_view bytes() const { return bytes_; }

 private:
  bool ReadVarint(uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64 && pos_ < data_.size(); shift += 7) {
      const uint8_t byte = static_cast<uint8_t>(data_[pos_++]);
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if (!(byte & 0x80)) {
        return true;
      }
    }
    return false;
  }

  bool Fail() {
    pos_ = data_.size();
    return false;
  }

  std::string_view data_;
  size_t pos_ = 0;
  int field_ = 0;
  int type_ = 0;
  uint64_t varint_ = 0;
  uint64_t fixed64_ = 0;
  std::string_view bytes_;
};

// google.protobuf.Timestamp, as seconds since the Unix epoch.
double DecodeTimestamp(std::string_view data) {
  int64_t seconds = 0;
  int64_t nanos = 0;
  WireReader reader(data);
  while (reader.Next()) {
    if (reader.field() == 1 && reader.is_varint()) {
      seconds = static_cast<int64_t>(reader.varint());
    } else if (reader.field() == 2 && reader.is_varint()) {
      nanos = static_cast<int32_t>(reader.varint());
    }
  }
  return static_cast<double>(seconds) + static_cast<double>(nanos) / 1e9;
}

base::DictValue DecodeFields(std::string_view data,
                             int entry_field,
                             int depth);

// google.firestore.v1.Value as a base::Value: maps are dicts, arrays lists,
// timestamps seconds since the Unix epoch; references, bytes and geo
// points (Arc's boards have none) are null.
base::Value DecodeValue(std::string_view data, int depth) {
  WireReader reader(data);
  while (reader.Next()) {
    switch (reader.field()) {
      case 1:  // boolean_value
        if (reader.is_varint()) {
          return base::Value(reader.varint() != 0);
        }
        break;
      case 2:  // integer_value
        if (reader.is_varint()) {
          const int64_t value = static_cast<int64_t>(reader.varint());
          if (value >= std::numeric_limits<int>::min() &&
              value <= std::numeric_limits<int>::max()) {
            return base::Value(static_cast<int>(value));
          }
          return base::Value(static_cast<double>(value));
        }
        break;
      case 3:  // double_value
        if (reader.is_fixed64()) {
          const double value = reader.fixed64_as_double();
          return std::isfinite(value) ? base::Value(value) : base::Value();
        }
        break;
      case 10:  // timestamp_value
        if (reader.is_bytes()) {
          return base::Value(DecodeTimestamp(reader.bytes()));
        }
        break;
      case 17:  // string_value
        if (reader.is_bytes()) {
          return base::IsStringUTF8AllowingNoncharacters(reader.bytes())
                     ? base::Value(reader.bytes())
                     : base::Value();
        }
        break;
      case 6:  // map_value
        if (reader.is_bytes() && depth < kMaxDepth) {
          return base::Value(DecodeFields(reader.bytes(), 1, depth + 1));
        }
        return base::Value();
      case 9:  // array_value
        if (reader.is_bytes() && depth < kMaxDepth) {
          base::ListValue list;
          WireReader values(reader.bytes());
          while (values.Next()) {
            if (values.field() == 1 && values.is_bytes()) {
              list.Append(DecodeValue(values.bytes(), depth + 1));
            }
          }
          return base::Value(std::move(list));
        }
        return base::Value();
      default:
        // null_value (11), bytes (18), reference (5), geo point (8).
        return base::Value();
    }
  }
  return base::Value();
}

// A map<string, Value> (MapValue.fields is field 1, Document.fields 2).
base::DictValue DecodeFields(std::string_view data,
                             int entry_field,
                             int depth) {
  base::DictValue fields;
  WireReader reader(data);
  while (reader.Next()) {
    if (reader.field() != entry_field || !reader.is_bytes()) {
      continue;
    }
    std::string_view key;
    std::string_view value;
    WireReader entry(reader.bytes());
    while (entry.Next()) {
      if (entry.field() == 1 && entry.is_bytes()) {
        key = entry.bytes();
      } else if (entry.field() == 2 && entry.is_bytes()) {
        value = entry.bytes();
      }
    }
    if (!key.empty() && base::IsStringUTF8AllowingNoncharacters(key)) {
      fields.Set(key, DecodeValue(value, depth));
    }
  }
  return fields;
}

// A cached document of Arc's.
struct Document {
  // After ".../documents/": "easels/<id>", "easels/<id>/objects/<id>".
  std::string path;
  base::DictValue fields;
  // Its update time, or for a deleted one when it was known deleted.
  double update_time = 0;
  bool deleted = false;
};

// A firestore.client.MaybeDocument: a google.firestore.v1.Document (field
// 2: name 1, fields 2, update_time 4), or a NoDocument (1: name 1,
// read_time 2) for one known deleted. nullopt for anything else
// (UnknownDocument, 3).
std::optional<Document> DecodeMaybeDocument(std::string_view data) {
  std::string_view message;
  bool deleted = false;
  WireReader reader(data);
  while (reader.Next()) {
    if ((reader.field() == 1 || reader.field() == 2) && reader.is_bytes()) {
      message = reader.bytes();
      deleted = reader.field() == 1;
    }
  }
  if (message.empty()) {
    return std::nullopt;
  }
  Document result;
  result.deleted = deleted;
  std::string_view name;
  WireReader parts(message);
  while (parts.Next()) {
    if (!parts.is_bytes()) {
      continue;
    }
    if (parts.field() == 1) {
      name = parts.bytes();
    } else if (parts.field() == (deleted ? 2 : 4)) {
      result.update_time = DecodeTimestamp(parts.bytes());
    }
  }
  constexpr std::string_view kDocuments = "/documents/";
  const size_t at = name.find(kDocuments);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  result.path = std::string(name.substr(at + kDocuments.size()));
  if (!deleted) {
    result.fields = DecodeFields(message, 2, 0);
  }
  return result;
}

// Arc's colors (the editor's palette names) as CSS colors. "black" is the
// board's ink (absent), so it stays readable on a dark board.
std::optional<std::string> ArcColor(std::string_view name) {
  static constexpr auto kColors =
      base::MakeFixedFlatMap<std::string_view, std::string_view>({
          {"blue", "#0a84ff"},         {"gray", "#8e8e93"},
          {"green", "#30a46c"},        {"grey", "#8e8e93"},
          {"lightblue", "#b9dcff"},    {"lightgray", "#e3e3e8"},
          {"lightgreen", "#bfe8cf"},   {"lightgrey", "#e3e3e8"},
          {"lightorange", "#ffd8b5"},  {"lightpink", "#ffd0e6"},
          {"lightpurple", "#e2cdf6"},  {"lightred", "#ffc9c9"},
          {"lightyellow", "#fff0a8"},  {"orange", "#ff8a1f"},
          {"pink", "#ff4fa3"},         {"purple", "#9b5de5"},
          {"red", "#ff3b30"},          {"white", "#ffffff"},
          {"yellow", "#ffcc00"},
      });
  const std::string lower = base::ToLowerASCII(name);
  if (const auto it = kColors.find(lower); it != kColors.end()) {
    return std::string(it->second);
  }
  return std::nullopt;
}

// Arc's text fonts: "clean" was seen; the others are its likely names for
// the same ideas.
std::optional<std::string_view> ArcFont(std::string_view name) {
  static constexpr auto kFonts =
      base::MakeFixedFlatMap<std::string_view, std::string_view>({
          {"casual", "hand"},     {"classic", "serif"},
          {"clean", "sans"},      {"code", "mono"},
          {"elegant", "serif"},   {"hand", "hand"},
          {"handwritten", "hand"}, {"marker", "hand"},
          {"mono", "mono"},       {"monospace", "mono"},
          {"playful", "hand"},    {"sans", "sans"},
          {"serif", "serif"},     {"technical", "mono"},
      });
  if (const auto it = kFonts.find(base::ToLowerASCII(name));
      it != kFonts.end()) {
    return it->second;
  }
  return std::nullopt;
}

// Arc's paragraph styles: "h2" (a board's title) and "body" were seen.
std::optional<std::string_view> ArcTextStyle(std::string_view name) {
  static constexpr auto kStyles =
      base::MakeFixedFlatMap<std::string_view, std::string_view>({
          {"body", "body"},       {"caption", "caption"},
          {"footnote", "caption"}, {"h1", "title"},
          {"h2", "heading"},      {"h3", "heading"},
          {"heading", "heading"}, {"small", "caption"},
          {"subtitle", "heading"}, {"title", "title"},
      });
  if (const auto it = kStyles.find(base::ToLowerASCII(name));
      it != kStyles.end()) {
    return it->second;
  }
  return std::nullopt;
}

// Arc names its board backgrounds ("vibrant" seen); Compass has paper,
// grid, dots and dark.
std::string ArcPalette(std::string_view name) {
  const std::string lower = base::ToLowerASCII(name);
  if (lower.find("dark") != std::string::npos ||
      lower.find("night") != std::string::npos) {
    return "dark";
  }
  return "paper";
}

int64_t ToSyncTime(base::Time time) {
  return time.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

base::Time FromUnixSeconds(double seconds) {
  return base::Time::FromSecondsSinceUnixEpoch(seconds);
}

std::optional<double> Number(const base::Value* value) {
  if (!value) {
    return std::nullopt;
  }
  std::optional<double> number = value->GetIfDouble();
  if (!number || !std::isfinite(*number)) {
    return std::nullopt;
  }
  return number;
}

// [x, y] or {x, y}.
std::optional<std::pair<double, double>> Pair(const base::Value* value) {
  if (!value) {
    return std::nullopt;
  }
  std::optional<double> x;
  std::optional<double> y;
  if (const base::ListValue* list = value->GetIfList();
      list && list->size() >= 2) {
    x = Number(&(*list)[0]);
    y = Number(&(*list)[1]);
  } else if (const base::DictValue* dict = value->GetIfDict()) {
    x = Number(dict->Find("x"));
    y = Number(dict->Find("y"));
  }
  if (!x || !y) {
    return std::nullopt;
  }
  return std::make_pair(*x, *y);
}

GURL WebUrl(const std::string* spec) {
  if (!spec) {
    return GURL();
  }
  GURL url(*spec);
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() ? url : GURL();
}

// An image's URL: {content: {url}}.
GURL ImageUrl(const base::DictValue* image) {
  if (!image) {
    return GURL();
  }
  const std::string* spec = image->FindStringByDottedPath("content.url");
  return spec ? GURL(*spec) : GURL();
}

std::string String(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

bool MapText(const base::DictValue& text, sync_pb::CompassEaselText& out) {
  const std::string* string = text.FindString("string");
  if (!string ||
      base::TrimWhitespaceASCII(*string, base::TRIM_ALL).empty()) {
    return false;
  }
  out.set_text(*string);
  if (const base::DictValue* style = text.FindDict("style")) {
    if (const std::string* font = style->FindString("fontFamily")) {
      if (std::optional<std::string_view> mapped = ArcFont(*font)) {
        out.set_font(std::string(*mapped));
      }
    }
    if (const std::string* paragraph = style->FindString("paragraphStyle")) {
      if (std::optional<std::string_view> mapped = ArcTextStyle(*paragraph)) {
        out.set_style(std::string(*mapped));
      }
    }
    // fillStyle ("none" seen) has no Compass counterpart.
  }
  return true;
}

// Arc's shapes: their points are fractions of the object's box.
bool MapShape(const base::DictValue& shape,
              double width,
              double height,
              sync_pb::CompassEaselShape& out) {
  const std::string color = String(shape, "color");
  if (std::optional<std::string> css = ArcColor(color)) {
    out.set_color(*css);
  }
  const base::DictValue* type = shape.FindDict("type");
  if (!type) {
    return false;
  }
  // Arc's light colors are backgrounds (a panel behind cards), its strong
  // ones outlines around what they point out.
  const bool filled = base::StartsWith(color, "light",
                                       base::CompareCase::INSENSITIVE_ASCII);
  if (type->FindBool("isRectangle").value_or(false)) {
    out.set_kind(1);
    out.set_filled(filled);
    return true;
  }
  if (type->FindBool("isCircle").value_or(false)) {
    out.set_kind(2);
    out.set_filled(filled);
    return true;
  }
  if (const base::DictValue* arrow = type->FindDict("arrow")) {
    std::optional<std::pair<double, double>> start = Pair(arrow->Find("start"));
    std::optional<std::pair<double, double>> end = Pair(arrow->Find("end"));
    if (!start || !end) {
      return false;
    }
    out.set_kind(3);
    out.add_points(start->first * width);
    out.add_points(start->second * height);
    out.add_points(end->first * width);
    out.add_points(end->second * height);
    return true;
  }
  if (const base::ListValue* points = type->FindList("freehand")) {
    out.set_kind(4);
    for (const base::Value& point : *points) {
      if (static_cast<size_t>(out.points_size()) >= 2 * kMaxRawPoints) {
        break;
      }
      if (std::optional<std::pair<double, double>> xy = Pair(&point)) {
        out.add_points(xy->first * width);
        out.add_points(xy->second * height);
      }
    }
    return out.points_size() >= 2;
  }
  return false;
}

// One of Arc's objects as Compass's, or nullopt for a kind it doesn't know
// or one with nothing to show.
std::optional<ArcEaselObject> MapObject(const std::string& id,
                                        const base::DictValue& fields) {
  const base::DictValue* rect = fields.FindDictByDottedPath("position.rect");
  const base::DictValue* content = fields.FindDict("content");
  if (!rect || !content) {
    return std::nullopt;
  }
  std::optional<double> x = Number(rect->Find("x"));
  std::optional<double> y = Number(rect->Find("y"));
  std::optional<double> width = Number(rect->Find("width"));
  std::optional<double> height = Number(rect->Find("height"));
  if (!x || !y || !width || !height) {
    return std::nullopt;
  }
  ArcEaselObject result;
  result.id = id;
  sync_pb::CompassEaselObject& object = result.object;
  object.set_x(*x * kArcEaselWidth);
  object.set_y(*y * kArcEaselWidth);
  object.set_width(std::max(0.0, *width * kArcEaselWidth));
  object.set_height(std::max(0.0, *height * kArcEaselWidth));
  object.set_z(Number(fields.FindByDottedPath("position.zIndex")).value_or(0));

  if (const base::DictValue* text = content->FindDict("text")) {
    if (!MapText(*text, *object.mutable_text())) {
      return std::nullopt;
    }
    return result;
  }
  if (const base::DictValue* shape = content->FindDict("shape")) {
    if (!MapShape(*shape, object.width(), object.height(),
                  *object.mutable_shape())) {
      return std::nullopt;
    }
    return result;
  }
  if (const base::DictValue* image = content->FindDict("image")) {
    result.image_url = ImageUrl(image);
    if (!result.image_url.is_valid()) {
      return std::nullopt;
    }
    sync_pb::CompassEaselImage& out = *object.mutable_image();
    if (std::optional<std::pair<double, double>> size =
            Pair(image->Find("size"))) {
      out.set_pixel_width(static_cast<int>(std::clamp(size->first, 0.0, 1e6)));
      out.set_pixel_height(
          static_cast<int>(std::clamp(size->second, 0.0, 1e6)));
    }
    return result;
  }
  if (const base::DictValue* link = content->FindDict("link")) {
    const GURL url = WebUrl(link->FindString("source"));
    if (!url.is_valid()) {
      return std::nullopt;
    }
    sync_pb::CompassEaselLink& out = *object.mutable_link();
    out.set_url(url.spec());
    out.set_title(String(*link, "title"));
    out.set_description(String(*link, "description"));
    result.image_url = ImageUrl(link->FindDict("image"));
    return result;
  }
  if (const base::DictValue* card = content->FindDict("webCard")) {
    const GURL url = WebUrl(card->FindString("url"));
    if (!url.is_valid()) {
      return std::nullopt;
    }
    sync_pb::CompassEaselCapture& out = *object.mutable_capture();
    out.set_url(url.spec());
    out.set_title(String(*card, "text"));
    result.image_url = ImageUrl(card->FindDict("image"));
    result.favicon_url = ImageUrl(card->FindDict("favicon"));
    if (const base::DictValue* layout = card->FindDict("captureLayoutInfo")) {
      // The region in the viewport's CSS pixels, the page's scroll offset
      // and the viewport's size, as Compass keeps them.
      if (const base::DictValue* frame =
              layout->FindDict("frameRelativeToViewport")) {
        out.set_region_x(Number(frame->Find("x")).value_or(0));
        out.set_region_y(Number(frame->Find("y")).value_or(0));
        out.set_region_width(Number(frame->Find("width")).value_or(0));
        out.set_region_height(Number(frame->Find("height")).value_or(0));
      }
      if (std::optional<std::pair<double, double>> offset =
              Pair(layout->Find("webContentOffset"))) {
        out.set_scroll_x(offset->first);
        out.set_scroll_y(offset->second);
      }
      if (std::optional<std::pair<double, double>> size =
              Pair(layout->Find("webContentSize"))) {
        out.set_viewport_width(size->first);
        out.set_viewport_height(size->second);
      }
    }
    if (std::optional<double> captured = Number(card->Find("dateCaptured"))) {
      out.set_capture_time(ToSyncTime(FromUnixSeconds(*captured)));
    }
    return result;
  }
  return std::nullopt;
}

void CollectEasels(const std::vector<Item>& items,
                   std::set<std::string>& seen,
                   std::vector<ArcEaselRef>& out) {
  for (const Item& item : items) {
    if (item.kind == Item::Kind::kEasel) {
      if (seen.insert(item.easel_id).second) {
        out.push_back({item.easel_id, item.saved_title});
      }
    }
    CollectEasels(item.children, seen, out);
  }
}

// Copies Arc's LevelDB |from| into |to|, without its lock and text logs.
bool CopyDatabase(const base::FilePath& from, const base::FilePath& to) {
  base::FileEnumerator files(from, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  bool any = false;
  for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
    const std::string name = file.BaseName().AsUTF8Unsafe();
    if (name == "LOCK" || name == "LOG" || name == "LOG.old") {
      continue;
    }
    if (!base::CopyFile(file, to.Append(file.BaseName()))) {
      return false;
    }
    any = true;
  }
  return any;
}

}  // namespace

std::vector<ArcEaselRef> EaselsIn(const std::vector<Space>& spaces) {
  std::vector<ArcEaselRef> easels;
  std::set<std::string> seen;
  for (const Space& space : spaces) {
    CollectEasels(space.pinned, seen, easels);
    CollectEasels(space.today, seen, easels);
  }
  return easels;
}

bool IsArcEaselImageUrl(const GURL& url) {
  return url.is_valid() && url.SchemeIs(url::kHttpsScheme) &&
         url.host() == kImageHost && url.EffectiveIntPort() == 443;
}

sync_pb::CompassEaselObject WithoutImages(const ArcEaselObject& object) {
  sync_pb::CompassEaselObject result = object.object;
  if (result.has_image()) {
    result.clear_image();
    if (object.image_url.is_valid()) {
      sync_pb::CompassEaselLink& link = *result.mutable_link();
      link.set_url(object.image_url.spec());
      link.set_title("Image from Arc");
      link.set_description("The image couldn't be brought over.");
    } else {
      result.mutable_text()->set_text("(An image from Arc)");
    }
  } else if (result.has_capture()) {
    sync_pb::CompassEaselCapture capture = result.capture();
    result.clear_capture();
    sync_pb::CompassEaselLink& link = *result.mutable_link();
    link.set_url(capture.url());
    link.set_title(capture.title());
  } else if (result.has_link()) {
    result.mutable_link()->clear_image_asset();
  }
  return result;
}

ArcEasels ReadArcEasels(const base::FilePath& dir) {
  ArcEasels easels;
  if (dir.empty() || !base::DirectoryExists(dir)) {
    return easels;
  }
  base::ScopedTempDir copy;
  if (!copy.CreateUniqueTempDir() || !CopyDatabase(dir, copy.GetPath())) {
    return easels;
  }
  const std::string path = copy.GetPath().AsUTF8Unsafe();
  leveldb_env::Options options;
  options.create_if_missing = false;
  options.paranoid_checks = false;
  std::unique_ptr<leveldb::DB> db;
  if (!leveldb_env::OpenDB(options, path, &db).ok()) {
    // No CURRENT or manifest (caught while Firestore was writing them):
    // rebuilt from the logs and tables that are there.
    if (!leveldb::RepairDB(path, options).ok() ||
        !leveldb_env::OpenDB(options, path, &db).ok()) {
      LOG(ERROR) << "Arc import: can't read Arc's Easels cache";
      return easels;
    }
  }

  // The latest version of each document.
  std::map<std::string, Document> documents;
  {
    std::unique_ptr<leveldb::Iterator> it(
        db->NewIterator(leveldb::ReadOptions()));
    for (it->Seek(leveldb::Slice(kRemoteDocumentPrefix.data(),
                                 kRemoteDocumentPrefix.size()));
         it->Valid(); it->Next()) {
      const std::string_view key(it->key().data(), it->key().size());
      if (!key.starts_with(kRemoteDocumentPrefix)) {
        break;
      }
      std::optional<Document> document = DecodeMaybeDocument(
          std::string_view(it->value().data(), it->value().size()));
      if (!document || !document->path.starts_with("easels/")) {
        continue;
      }
      auto existing = documents.find(document->path);
      if (existing == documents.end() ||
          existing->second.update_time <= document->update_time) {
        documents[document->path] = std::move(*document);
      }
    }
  }
  db.reset();
  std::erase_if(documents,
                [](const auto& entry) { return entry.second.deleted; });

  // Boards first, then their objects ("easels/<id>/objects/<id>"); members
  // and other subcollections are left out.
  std::map<std::string, std::string> title_objects;
  for (auto& [doc_path, document] : documents) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        doc_path, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    if (parts.size() != 2 || parts[1].empty()) {
      continue;
    }
    ArcEasel& easel = easels[std::string(parts[1])];
    easel.id = std::string(parts[1]);
    easel.title = String(document.fields, "title");
    easel.palette = ArcPalette(String(document.fields, "palette"));
    std::optional<double> modified =
        Number(document.fields.Find("lastModified"));
    easel.last_modified =
        FromUnixSeconds(modified.value_or(document.update_time));
    title_objects[easel.id] = String(document.fields, "titleObjectID");
  }
  for (auto& [doc_path, document] : documents) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        doc_path, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    if (parts.size() != 4 || parts[2] != "objects" || parts[1].empty() ||
        parts[3].empty()) {
      continue;
    }
    ArcEasel& easel = easels[std::string(parts[1])];
    if (easel.id.empty()) {
      // Its objects without the board's own document.
      easel.id = std::string(parts[1]);
      easel.palette = ArcPalette("");
    }
    easel.has_objects = true;
    const std::string object_id(parts[3]);
    const std::string& title_object = title_objects[easel.id];
    const std::string* own_id =
        document.fields.FindStringByDottedPath("id.value");
    if (!title_object.empty() &&
        (object_id == title_object || (own_id && *own_id == title_object))) {
      continue;
    }
    if (std::optional<ArcEaselObject> object =
            MapObject(object_id, document.fields)) {
      easel.objects.push_back(std::move(*object));
    }
  }
  for (auto& [id, easel] : easels) {
    std::ranges::stable_sort(easel.objects, [](const ArcEaselObject& a,
                                               const ArcEaselObject& b) {
      return a.object.z() < b.object.z();
    });
  }
  return easels;
}

}  // namespace compass::arc
