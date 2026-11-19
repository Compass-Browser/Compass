// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/easels/easel_store.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/types/expected_macros.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "crypto/hash.h"

namespace compass::easels {

namespace {

constexpr base::TimeDelta kCommitInterval = base::Seconds(1);
constexpr char kDirName[] = "Compass Easels";
constexpr char kFileName[] = "easels.pb";
constexpr char kAssetsDir[] = "assets";
constexpr std::string_view kEaselUrlPrefixes[] = {
    "compass://easel/", "chrome://easel/"};

bool IsLowerUuid(std::string_view text) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(text);
  return uuid.is_valid();
}

bool IsSha256Hex(std::string_view text) {
  return text.size() == 64 &&
         std::ranges::all_of(text, [](char c) {
           return base::IsAsciiDigit(c) || (c >= 'a' && c <= 'f');
         });
}

bool IsKnownMime(std::string_view mime) {
  return mime == "image/png" || mime == "image/jpeg" || mime == "image/webp";
}

bool IsFinite(double value) {
  return std::isfinite(value);
}

// Truncates to |max| characters, at a character boundary.
std::string TruncateChars(const std::string& text, size_t max) {
  std::u16string wide = base::UTF8ToUTF16(text);
  if (wide.size() <= max) {
    return text;
  }
  wide.resize(max);
  // Don't leave half a surrogate pair.
  if (!wide.empty() && (wide.back() & 0xF800) == 0xD800) {
    wide.pop_back();
  }
  return base::UTF16ToUTF8(wide);
}

// Keeps every n-th point of a stroke so it has at most |max| points, the
// last one always kept.
void SimplifyStroke(google::protobuf::RepeatedField<double>& points,
                    size_t max) {
  const size_t count = points.size() / 2;
  if (count <= max) {
    return;
  }
  const double step = static_cast<double>(count - 1) / (max - 1);
  google::protobuf::RepeatedField<double> kept;
  for (size_t i = 0; i < max; ++i) {
    const size_t index =
        std::min(count - 1, static_cast<size_t>(std::lround(i * step)));
    kept.Add(points[index * 2]);
    kept.Add(points[index * 2 + 1]);
  }
  points.Swap(&kept);
}

int ContentKinds(const sync_pb::CompassEaselObject& object) {
  return object.has_text() + object.has_shape() + object.has_image() +
         object.has_link() + object.has_capture();
}

// Every image a board's objects point at.
void AddAssetsOf(const sync_pb::CompassEaselObject& object,
                 std::set<std::string>& out) {
  if (object.has_image()) {
    out.insert(object.image().asset());
  }
  if (object.has_link() && object.link().has_image_asset()) {
    out.insert(object.link().image_asset());
  }
  if (object.has_capture()) {
    out.insert(object.capture().image_asset());
    if (object.capture().has_favicon_asset()) {
      out.insert(object.capture().favicon_asset());
    }
  }
}

// The file format: records, each a varint length then a
// CompassSpacesSpecifics with one board, object or image (an image without
// its bytes, which are in assets/).
void AppendVarint(std::string& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

bool ReadVarint(std::string_view data, size_t& pos, uint64_t& value) {
  value = 0;
  for (int shift = 0; shift < 64 && pos < data.size(); shift += 7) {
    const uint8_t byte = static_cast<uint8_t>(data[pos++]);
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      return true;
    }
  }
  return false;
}

void AppendRecord(std::string& out,
                  const sync_pb::CompassSpacesSpecifics& specifics) {
  const std::string bytes = specifics.SerializeAsString();
  AppendVarint(out, bytes.size());
  out += bytes;
}

}  // namespace

struct EaselStore::ReadResult {
  std::vector<sync_pb::CompassEasel> easels;
  std::vector<sync_pb::CompassEaselObject> objects;
  std::vector<sync_pb::CompassEaselAsset> assets;
};

namespace {

std::unique_ptr<EaselStore::ReadResult> ReadStore(const base::FilePath& dir) {
  auto result = std::make_unique<EaselStore::ReadResult>();
  // The writer doesn't make the folder.
  base::CreateDirectory(dir);
  std::string contents;
  if (!base::ReadFileToString(dir.AppendASCII(kFileName), &contents)) {
    return result;
  }
  const std::string_view data(contents);
  size_t pos = 0;
  while (pos < data.size()) {
    uint64_t size = 0;
    if (!ReadVarint(data, pos, size) || size > data.size() - pos) {
      LOG(WARNING) << "Compass Easels: " << kFileName << " is cut short";
      break;
    }
    sync_pb::CompassSpacesSpecifics specifics;
    if (specifics.ParseFromString(data.substr(pos, size))) {
      if (specifics.has_easel()) {
        result->easels.push_back(specifics.easel());
      } else if (specifics.has_easel_object()) {
        result->objects.push_back(specifics.easel_object());
      } else if (specifics.has_easel_asset()) {
        sync_pb::CompassEaselAsset asset = specifics.easel_asset();
        std::string bytes;
        const base::FilePath path =
            dir.AppendASCII(kAssetsDir).AppendASCII(asset.sha256());
        if (IsSha256Hex(asset.sha256()) &&
            base::ReadFileToString(path, &bytes) &&
            Sha256Hex(bytes) == asset.sha256()) {
          asset.set_data(std::move(bytes));
          result->assets.push_back(std::move(asset));
        }
      }
    }
    pos += size;
  }
  return result;
}

void WriteAsset(const base::FilePath& dir,
                const std::string& sha256,
                const std::string& data) {
  const base::FilePath assets = dir.AppendASCII(kAssetsDir);
  if (!base::CreateDirectory(assets)) {
    return;
  }
  const base::FilePath path = assets.AppendASCII(sha256);
  if (!base::PathExists(path)) {
    base::ImportantFileWriter::WriteFileAtomically(path, data,
                                                   "CompassEaselAsset");
  }
}

void DeleteAssetFiles(const base::FilePath& dir,
                      const std::vector<std::string>& sha256s) {
  for (const std::string& sha : sha256s) {
    base::DeleteFile(dir.AppendASCII(kAssetsDir).AppendASCII(sha));
  }
}

}  // namespace

std::string EaselUrl(std::string_view uuid) {
  return std::string(kEaselUrlPrefixes[0]) + base::ToLowerASCII(uuid);
}

std::string EaselUuidFromUrl(std::string_view url) {
  for (std::string_view prefix : kEaselUrlPrefixes) {
    if (base::StartsWith(url, prefix, base::CompareCase::INSENSITIVE_ASCII)) {
      std::string rest = base::ToLowerASCII(url.substr(prefix.size()));
      const size_t end = rest.find_first_of("?#/");
      if (end != std::string::npos) {
        rest.resize(end);
      }
      return IsLowerUuid(rest) ? rest : std::string();
    }
  }
  return std::string();
}

std::string Sha256Hex(std::string_view data) {
  return base::HexEncodeLower(crypto::hash::Sha256(base::as_byte_span(data)));
}

std::string AssetUuid(std::string_view sha256) {
  const std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256(std::string("compass-easel-asset:") +
                           std::string(sha256));
  std::array<uint8_t, 16> b;
  std::copy_n(digest.begin(), 16, b.begin());
  b[6] = (b[6] & 0x0f) | 0x50;
  b[8] = (b[8] & 0x3f) | 0x80;
  const std::string hex = base::HexEncodeLower(b);
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
         "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

int64_t ToSyncTime(base::Time time) {
  return time.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

base::Time FromSyncTime(int64_t micros) {
  return base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(micros));
}

base::expected<sync_pb::CompassEaselObject, std::string> NormalizeObject(
    sync_pb::CompassEaselObject object) {
  object.set_uuid(base::ToLowerASCII(object.uuid()));
  object.set_easel_uuid(base::ToLowerASCII(object.easel_uuid()));
  if (!IsLowerUuid(object.easel_uuid())) {
    return base::unexpected("The object has no board.");
  }
  for (double value : {object.x(), object.y(), object.width(),
                       object.height(), object.z()}) {
    if (!IsFinite(value) || std::abs(value) > 1e7) {
      return base::unexpected("The object's position is out of range.");
    }
  }
  if (object.width() < 0 || object.height() < 0) {
    return base::unexpected("The object's size can't be negative.");
  }
  if (ContentKinds(object) != 1) {
    // One this version doesn't know is kept as it is (from sync); a user's
    // must have one.
    if (ContentKinds(object) == 0 && !object.unknown_fields().empty()) {
      return object;
    }
    return base::unexpected("An object has exactly one kind of content.");
  }
  if (object.has_text()) {
    sync_pb::CompassEaselText& text = *object.mutable_text();
    text.set_text(TruncateChars(text.text(), kMaxTextChars));
  }
  if (object.has_shape()) {
    sync_pb::CompassEaselShape& shape = *object.mutable_shape();
    if (!std::ranges::all_of(shape.points(), IsFinite)) {
      return base::unexpected("A shape's points must be numbers.");
    }
    if (shape.points_size() % 2) {
      shape.mutable_points()->RemoveLast();
    }
    if (shape.kind() == kArrow && shape.points_size() != 4) {
      return base::unexpected("An arrow has a start and an end.");
    }
    if (shape.kind() == kFreehand) {
      if (shape.points_size() < 2) {
        return base::unexpected("A stroke needs points.");
      }
      SimplifyStroke(*shape.mutable_points(), kMaxFreehandPoints);
    }
    if (shape.has_stroke_width() &&
        (!IsFinite(shape.stroke_width()) || shape.stroke_width() < 0 ||
         shape.stroke_width() > 200)) {
      shape.clear_stroke_width();
    }
  }
  if (object.has_image() && !IsSha256Hex(object.image().asset())) {
    return base::unexpected("The image is missing.");
  }
  if (object.has_capture() && !IsSha256Hex(object.capture().image_asset())) {
    return base::unexpected("The capture's image is missing.");
  }
  return object;
}

EaselStore::Entities::Entities() = default;
EaselStore::Entities::Entities(Entities&&) = default;
EaselStore::Entities& EaselStore::Entities::operator=(Entities&&) = default;
EaselStore::Entities::~Entities() = default;

EaselStore::EaselStore(const base::FilePath& dir)
    : dir_(dir),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      writer_(dir.AppendASCII(kFileName),
              file_task_runner_,
              kCommitInterval,
              "CompassEasels") {
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadStore, dir_),
      base::BindOnce(&EaselStore::OnRead, weak_factory_.GetWeakPtr()));
}

EaselStore::~EaselStore() {
  Flush();
}

// static
EaselStore* EaselStore::Get(Profile* profile) {
  return EaselStoreFactory::GetForProfile(profile);
}

void EaselStore::WhenLoaded(base::OnceClosure done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (loaded_) {
    std::move(done).Run();
    return;
  }
  when_loaded_.push_back(std::move(done));
}

void EaselStore::OnRead(std::unique_ptr<ReadResult> result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // What sync applied before the files were read waits in when_loaded_ and
  // runs after this.
  for (sync_pb::CompassEasel& easel : result->easels) {
    if (IsLowerUuid(easel.uuid())) {
      easels_[easel.uuid()] = std::move(easel);
    }
  }
  for (sync_pb::CompassEaselObject& object : result->objects) {
    if (IsLowerUuid(object.uuid())) {
      objects_[object.uuid()] = std::move(object);
    }
  }
  for (sync_pb::CompassEaselAsset& asset : result->assets) {
    assets_[asset.sha256()] = std::move(asset);
  }
  loaded_ = true;
  DropUnusedAssets();
  std::vector<base::OnceClosure> callbacks = std::move(when_loaded_);
  for (base::OnceClosure& callback : callbacks) {
    std::move(callback).Run();
  }
  for (Observer& observer : observers_) {
    observer.OnEaselChanged(std::string());
    observer.OnEaselsChanged();
  }
}

std::vector<const sync_pb::CompassEasel*> EaselStore::Easels() const {
  std::vector<const sync_pb::CompassEasel*> list;
  for (const auto& [uuid, easel] : easels_) {
    list.push_back(&easel);
  }
  std::ranges::sort(list, [](const sync_pb::CompassEasel* a,
                             const sync_pb::CompassEasel* b) {
    return a->create_time() != b->create_time()
               ? a->create_time() > b->create_time()
               : a->uuid() < b->uuid();
  });
  return list;
}

const sync_pb::CompassEasel* EaselStore::FindEasel(
    std::string_view uuid) const {
  auto it = easels_.find(base::ToLowerASCII(uuid));
  return it == easels_.end() ? nullptr : &it->second;
}

std::vector<const sync_pb::CompassEaselObject*> EaselStore::ObjectsOf(
    std::string_view easel_uuid) const {
  const std::string key = base::ToLowerASCII(easel_uuid);
  std::vector<const sync_pb::CompassEaselObject*> list;
  for (const auto& [uuid, object] : objects_) {
    if (object.easel_uuid() == key) {
      list.push_back(&object);
    }
  }
  std::ranges::stable_sort(list, [](const sync_pb::CompassEaselObject* a,
                                    const sync_pb::CompassEaselObject* b) {
    return a->z() != b->z() ? a->z() < b->z()
                            : a->create_time() < b->create_time();
  });
  return list;
}

const sync_pb::CompassEaselObject* EaselStore::FindObject(
    std::string_view uuid) const {
  auto it = objects_.find(base::ToLowerASCII(uuid));
  return it == objects_.end() ? nullptr : &it->second;
}

const sync_pb::CompassEaselAsset* EaselStore::FindAsset(
    std::string_view sha256) const {
  auto it = assets_.find(std::string(sha256));
  return it == assets_.end() ? nullptr : &it->second;
}

base::expected<sync_pb::CompassEasel, std::string> EaselStore::CreateEasel(
    std::string title,
    std::string uuid) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_) {
    return base::unexpected("Easels are still loading. Try again in a moment.");
  }
  if (easels_.size() >= kMaxEasels) {
    return base::unexpected(
        "There are already 200 Easels. Delete one that's no longer needed "
        "first.");
  }
  uuid = uuid.empty() ? base::Uuid::GenerateRandomV4().AsLowercaseString()
                      : base::ToLowerASCII(uuid);
  if (!IsLowerUuid(uuid) || easels_.contains(uuid)) {
    return base::unexpected("That Easel already exists.");
  }
  sync_pb::CompassEasel easel;
  easel.set_uuid(uuid);
  easel.set_title(TruncateChars(
      std::string(base::TrimWhitespaceASCII(title, base::TRIM_ALL)),
      kMaxTitleChars));
  const int64_t now = ToSyncTime(base::Time::Now());
  easel.set_create_time(now);
  easel.set_update_time(now);
  easels_[uuid] = easel;
  Changed(uuid);
  return easel;
}

bool EaselStore::RenameEasel(std::string_view uuid, std::string title) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = easels_.find(base::ToLowerASCII(uuid));
  if (it == easels_.end()) {
    return false;
  }
  title = TruncateChars(
      std::string(base::TrimWhitespaceASCII(title, base::TRIM_ALL)),
      kMaxTitleChars);
  if (it->second.title() != title) {
    it->second.set_title(title);
    it->second.set_update_time(std::max(ToSyncTime(base::Time::Now()),
                                        it->second.update_time() + 1));
    Changed(it->first);
  }
  return true;
}

bool EaselStore::SetPalette(std::string_view uuid, std::string palette) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = easels_.find(base::ToLowerASCII(uuid));
  if (it == easels_.end()) {
    return false;
  }
  if (it->second.palette() != palette) {
    it->second.set_palette(palette.substr(0, 32));
    it->second.set_update_time(std::max(ToSyncTime(base::Time::Now()),
                                        it->second.update_time() + 1));
    Changed(it->first);
  }
  return true;
}

bool EaselStore::DeleteEasel(std::string_view uuid) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::string key = base::ToLowerASCII(uuid);
  if (!easels_.erase(key)) {
    return false;
  }
  std::erase_if(objects_, [&key](const auto& entry) {
    return entry.second.easel_uuid() == key;
  });
  // Its images stay until the next start's DropUnusedAssets(), so Undo can
  // bring objects back.
  Changed(key);
  return true;
}

base::expected<sync_pb::CompassEaselObject, std::string>
EaselStore::SaveObject(sync_pb::CompassEaselObject object) {
  std::vector<sync_pb::CompassEaselObject> list;
  list.push_back(std::move(object));
  base::expected<std::vector<sync_pb::CompassEaselObject>, std::string> saved =
      SaveObjects(std::move(list));
  if (!saved.has_value()) {
    return base::unexpected(saved.error());
  }
  return std::move(saved->front());
}

base::expected<std::vector<sync_pb::CompassEaselObject>, std::string>
EaselStore::SaveObjects(std::vector<sync_pb::CompassEaselObject> objects) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_) {
    return base::unexpected("Easels are still loading. Try again in a moment.");
  }
  // Checked first, all or nothing.
  std::vector<sync_pb::CompassEaselObject> normalized;
  std::map<std::string, size_t> added_per_easel;
  for (sync_pb::CompassEaselObject& object : objects) {
    ASSIGN_OR_RETURN(sync_pb::CompassEaselObject checked,
                     NormalizeObject(std::move(object)));
    if (!easels_.contains(checked.easel_uuid())) {
      return base::unexpected("There's no such Easel.");
    }
    if (checked.uuid().empty() || !objects_.contains(checked.uuid())) {
      if (!checked.uuid().empty() && !IsLowerUuid(checked.uuid())) {
        return base::unexpected("An object's id must be a UUID.");
      }
      const size_t count = ObjectsOf(checked.easel_uuid()).size() +
                           ++added_per_easel[checked.easel_uuid()];
      if (count > kMaxObjectsPerEasel) {
        return base::unexpected(
            "This Easel is full (1,000 objects). Delete some first.");
      }
    }
    std::set<std::string> images;
    AddAssetsOf(checked, images);
    for (const std::string& sha : images) {
      if (!assets_.contains(sha)) {
        return base::unexpected("An image of the object is missing.");
      }
    }
    normalized.push_back(std::move(checked));
  }
  const int64_t now = ToSyncTime(base::Time::Now());
  std::set<std::string> changed_easels;
  for (sync_pb::CompassEaselObject& object : normalized) {
    if (object.uuid().empty()) {
      object.set_uuid(base::Uuid::GenerateRandomV4().AsLowercaseString());
    }
    auto it = objects_.find(object.uuid());
    if (it == objects_.end()) {
      object.set_create_time(now);
      object.set_update_time(now);
    } else {
      object.set_create_time(it->second.create_time());
      object.set_update_time(it->second.update_time());
      if (object.SerializeAsString() == it->second.SerializeAsString()) {
        continue;
      }
      object.set_update_time(std::max(now, it->second.update_time() + 1));
    }
    changed_easels.insert(object.easel_uuid());
    objects_[object.uuid()] = object;
  }
  if (!changed_easels.empty()) {
    for (const std::string& easel : changed_easels) {
      Changed(easel);
    }
  }
  return normalized;
}

bool EaselStore::DeleteObject(std::string_view uuid) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = objects_.find(base::ToLowerASCII(uuid));
  if (it == objects_.end()) {
    return false;
  }
  const std::string easel = it->second.easel_uuid();
  objects_.erase(it);
  Changed(easel);
  return true;
}

base::expected<std::string, std::string> EaselStore::AddAsset(
    std::string data,
    std::string mime_type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_) {
    return base::unexpected("Easels are still loading. Try again in a moment.");
  }
  if (data.empty() || data.size() > kMaxAssetBytes) {
    return base::unexpected("The image is too large.");
  }
  if (!IsKnownMime(mime_type)) {
    return base::unexpected("Only PNG, JPEG and WebP images can be added.");
  }
  std::string sha = Sha256Hex(data);
  if (assets_.contains(sha)) {
    return base::ok(sha);
  }
  if (AssetBytes() + data.size() > kMaxAssetBytesTotal) {
    return base::unexpected(
        "Easels hold 64 MB of images, which is used up. Delete some images "
        "first.");
  }
  sync_pb::CompassEaselAsset asset;
  asset.set_uuid(AssetUuid(sha));
  asset.set_sha256(sha);
  asset.set_mime_type(mime_type);
  asset.set_create_time(ToSyncTime(base::Time::Now()));
  file_task_runner_->PostTask(FROM_HERE,
                              base::BindOnce(&WriteAsset, dir_, sha, data));
  asset.set_data(std::move(data));
  assets_[sha] = std::move(asset);
  // The object that uses it says which board changed.
  Changed(std::string());
  return base::ok(std::move(sha));
}

std::optional<EaselStore::Entities> EaselStore::GetEntities() const {
  if (!loaded_) {
    return std::nullopt;
  }
  Entities entities;
  for (const auto& [uuid, easel] : easels_) {
    entities.easels.push_back(easel);
  }
  for (const auto& [uuid, object] : objects_) {
    entities.objects.push_back(object);
  }
  for (const auto& [sha, asset] : assets_) {
    sync_pb::CompassEaselAsset& entity = entities.assets.emplace_back();
    entity.set_uuid(asset.uuid());
    entity.set_sha256(asset.sha256());
    entity.set_mime_type(asset.mime_type());
    entity.set_create_time(asset.create_time());
  }
  return entities;
}

void EaselStore::ApplyRemote(Entities changed,
                             const std::set<std::string>& deleted) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_) {
    when_loaded_.push_back(base::BindOnce(
        [](base::WeakPtr<EaselStore> store, Entities changed,
           std::set<std::string> deleted) {
          if (store) {
            store->ApplyRemote(std::move(changed), deleted);
          }
        },
        weak_factory_.GetWeakPtr(), std::move(changed), deleted));
    return;
  }
  bool any = false;
  for (sync_pb::CompassEasel& easel : changed.easels) {
    if (!IsLowerUuid(easel.uuid())) {
      continue;
    }
    auto it = easels_.find(easel.uuid());
    if (it == easels_.end() ||
        (easel.update_time() >= it->second.update_time() &&
         easel.SerializeAsString() != it->second.SerializeAsString())) {
      easels_[easel.uuid()] = std::move(easel);
      any = true;
    }
  }
  for (sync_pb::CompassEaselObject& object : changed.objects) {
    if (!IsLowerUuid(object.uuid())) {
      continue;
    }
    auto it = objects_.find(object.uuid());
    if (it == objects_.end() ||
        (object.update_time() >= it->second.update_time() &&
         object.SerializeAsString() != it->second.SerializeAsString())) {
      objects_[object.uuid()] = std::move(object);
      any = true;
    }
  }
  for (sync_pb::CompassEaselAsset& asset : changed.assets) {
    // Kept once: an image never changes. One whose bytes don't match its
    // hash, or that's too large, is left out.
    if (!IsSha256Hex(asset.sha256()) || assets_.contains(asset.sha256()) ||
        asset.data().size() > kMaxAssetBytes ||
        Sha256Hex(asset.data()) != asset.sha256()) {
      continue;
    }
    file_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&WriteAsset, dir_, asset.sha256(), asset.data()));
    assets_[asset.sha256()] = std::move(asset);
    any = true;
  }
  if (!deleted.empty()) {
    const size_t before = easels_.size() + objects_.size() + assets_.size();
    std::erase_if(easels_, [&deleted](const auto& entry) {
      return deleted.contains(entry.first);
    });
    std::erase_if(objects_, [&deleted](const auto& entry) {
      return deleted.contains(entry.first);
    });
    std::vector<std::string> gone;
    std::erase_if(assets_, [&](const auto& entry) {
      if (deleted.contains(entry.second.uuid())) {
        gone.push_back(entry.first);
        return true;
      }
      return false;
    });
    if (!gone.empty()) {
      file_task_runner_->PostTask(
          FROM_HERE, base::BindOnce(&DeleteAssetFiles, dir_, std::move(gone)));
    }
    any |= easels_.size() + objects_.size() + assets_.size() != before;
  }
  if (any) {
    Changed(std::string());
  }
}

// At load: images no object points at, older than kUnusedAssetLifetime. Not
// on each delete, so an undone delete finds its image.
void EaselStore::DropUnusedAssets() {
  std::set<std::string> used;
  for (const auto& [uuid, object] : objects_) {
    AddAssetsOf(object, used);
  }
  const int64_t cutoff =
      ToSyncTime(base::Time::Now() - kUnusedAssetLifetime);
  std::vector<std::string> gone;
  std::erase_if(assets_, [&](const auto& entry) {
    if (!used.contains(entry.first) && entry.second.create_time() < cutoff) {
      gone.push_back(entry.first);
      return true;
    }
    return false;
  });
  if (!gone.empty()) {
    file_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&DeleteAssetFiles, dir_, std::move(gone)));
  }
}

size_t EaselStore::AssetBytes() const {
  size_t total = 0;
  for (const auto& [sha, asset] : assets_) {
    total += asset.data().size();
  }
  return total;
}

void EaselStore::Changed(const std::string& easel_uuid) {
  if (loaded_) {
    writer_.ScheduleWrite(this);
  }
  for (Observer& observer : observers_) {
    observer.OnEaselChanged(easel_uuid);
    observer.OnEaselsChanged();
  }
}

void EaselStore::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void EaselStore::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void EaselStore::Flush() {
  if (writer_.HasPendingWrite()) {
    writer_.DoScheduledWrite();
  }
}

std::optional<std::string> EaselStore::SerializeData() {
  std::string out;
  for (const auto& [uuid, easel] : easels_) {
    sync_pb::CompassSpacesSpecifics specifics;
    *specifics.mutable_easel() = easel;
    AppendRecord(out, specifics);
  }
  for (const auto& [uuid, object] : objects_) {
    sync_pb::CompassSpacesSpecifics specifics;
    *specifics.mutable_easel_object() = object;
    AppendRecord(out, specifics);
  }
  for (const auto& [sha, asset] : assets_) {
    sync_pb::CompassSpacesSpecifics specifics;
    *specifics.mutable_easel_asset() = asset;
    specifics.mutable_easel_asset()->clear_data();
    AppendRecord(out, specifics);
  }
  return out;
}

void EaselStore::Shutdown() {
  Flush();
}

// static
EaselStore* EaselStoreFactory::GetForProfile(Profile* profile) {
  return static_cast<EaselStore*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
EaselStoreFactory* EaselStoreFactory::GetInstance() {
  static base::NoDestructor<EaselStoreFactory> instance;
  return instance.get();
}

EaselStoreFactory::EaselStoreFactory()
    : ProfileKeyedServiceFactory(
          "CompassEaselStore",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

EaselStoreFactory::~EaselStoreFactory() = default;

std::unique_ptr<KeyedService>
EaselStoreFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<EaselStore>(
      Profile::FromBrowserContext(context)->GetPath().AppendASCII(kDirName));
}

bool EaselStoreFactory::ServiceIsCreatedWithBrowserContext() const {
  // Sync applies remote boards at startup.
  return true;
}

}  // namespace compass::easels
