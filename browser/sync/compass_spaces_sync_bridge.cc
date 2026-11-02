// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/compass_spaces_sync_bridge.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string_view>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "crypto/hash.h"
#include "components/sync/base/client_tag_hash.h"
#include "components/sync/base/data_type.h"
#include "components/sync/base/deletion_origin.h"
#include "components/sync/model/conflict_resolution.h"
#include "components/sync/model/data_type_local_change_processor.h"
#include "components/sync/model/entity_change.h"
#include "components/sync/model/metadata_batch.h"
#include "components/sync/model/metadata_change_list.h"
#include "components/sync/model/mutable_data_batch.h"
#include "components/sync/protocol/entity_data.h"
#include "components/sync/protocol/entity_specifics.pb.h"
#include "components/sync/protocol/unique_position.pb.h"
#include "url/gurl.h"

namespace compass::sync {

std::string FaviconKey(std::string_view host) {
  const std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256("compass-favicon:" + base::ToLowerASCII(host));
  std::array<uint8_t, 16> b;
  std::copy_n(digest.begin(), 16, b.begin());
  b[6] = (b[6] & 0x0f) | 0x50;
  b[8] = (b[8] & 0x3f) | 0x80;
  const std::string hex = base::HexEncodeLower(b);
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
         "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

namespace {

using Type = SidebarNode::Type;

// update_time's encoding: microseconds since the Windows epoch.
int64_t ToWindowsMicros(base::Time time) {
  return time.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

base::Time FromWindowsMicros(int64_t micros) {
  return base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(micros));
}

// The host an entry's URL points at, lowercase, or empty.
std::string HostOf(const GURL& url) {
  return url.is_valid() && url.has_host() ? base::ToLowerASCII(url.host())
                                          : std::string();
}

std::string Key(const base::Uuid& id) {
  return id.AsLowercaseString();
}

base::Uuid ParseKey(const std::string& key) {
  return base::Uuid::ParseLowercase(key);
}

syncer::UniquePosition::Suffix SuffixFor(const std::string& key) {
  return syncer::UniquePosition::GenerateSuffix(
      syncer::ClientTagHash::FromUnhashed(syncer::COMPASS_SPACES, key));
}

std::optional<syncer::UniquePosition> ValidPosition(
    const sync_pb::UniquePosition& proto) {
  syncer::UniquePosition position = syncer::UniquePosition::FromProto(proto);
  if (!position.IsValid()) {
    return std::nullopt;
  }
  return position;
}

std::optional<Type> TypeFromProto(int type) {
  switch (type) {
    case kNodeTab:
      return Type::kTab;
    case kNodeFolder:
      return Type::kFolder;
    case kNodeSplit:
      return Type::kSplit;
    default:
      return std::nullopt;
  }
}

int TypeToProto(Type type) {
  switch (type) {
    case Type::kTab:
      return kNodeTab;
    case Type::kFolder:
      return kNodeFolder;
    case Type::kSplit:
      return kNodeSplit;
    case Type::kRoot:
      break;
  }
  return 0;
}

// Sets a string field to |value|, or clears it for an empty one. Leaves it
// alone if it already reads as |value|, so an explicitly empty field from
// another client doesn't count as a change.
#define COMPASS_SET_STRING(proto, field, value) \
  do {                                          \
    const std::string& v = (value);             \
    if ((proto).field() != v) {                 \
      if (v.empty()) {                          \
        (proto).clear_##field();                \
      } else {                                  \
        (proto).set_##field(v);                 \
      }                                         \
    }                                           \
  } while (false)

// The node's own fields: what it shows, not where it is.
void FillNodeContent(const SidebarNode& node, sync_pb::CompassNode& proto) {
  const bool tab = node.type() == Type::kTab;
  COMPASS_SET_STRING(proto, url,
                     tab ? node.url().possibly_invalid_spec() : std::string());
  COMPASS_SET_STRING(proto, title, base::UTF16ToUTF8(node.title()));
  COMPASS_SET_STRING(proto, custom_title,
                     tab ? base::UTF16ToUTF8(node.custom_title())
                         : std::string());
  const bool collapsed = node.type() == Type::kFolder && node.collapsed();
  if (proto.collapsed() != collapsed) {
    if (collapsed) {
      proto.set_collapsed(true);
    } else {
      proto.clear_collapsed();
    }
  }
}

// last_active_time and archived_time: microseconds since the Windows epoch.
int64_t ToProtoTime(base::Time time) {
  return time.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

base::Time FromProtoTime(int64_t value) {
  return base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(value));
}

// A Today tab's last-active time, once it has moved on enough from what the
// server has.
void FillLastActive(const SidebarNode& node, sync_pb::CompassNode& proto) {
  if (node.type() != Type::kTab || node.last_active().is_null()) {
    return;
  }
  const int64_t local = ToProtoTime(node.last_active());
  if (!proto.has_last_active_time() ||
      local - proto.last_active_time() >=
          kLastActiveGranularity.InMicroseconds()) {
    proto.set_last_active_time(local);
  }
}

// Whether |local| differs from |synced| only in what changes as a page is
// used: its URL, title and last-active time. Tabs only: a folder's title is
// its name.
bool OnlyContentDiffers(const sync_pb::CompassSpacesSpecifics& local,
                        const sync_pb::CompassSpacesSpecifics& synced) {
  if (!local.has_node() || !synced.has_node() ||
      local.node().type() != kNodeTab) {
    return false;
  }
  sync_pb::CompassSpacesSpecifics merged = synced;
  sync_pb::CompassNode& node = *merged.mutable_node();
  const sync_pb::CompassNode& from = local.node();
  if (from.has_url()) {
    node.set_url(from.url());
  } else {
    node.clear_url();
  }
  if (from.has_title()) {
    node.set_title(from.title());
  } else {
    node.clear_title();
  }
  if (from.has_last_active_time()) {
    node.set_last_active_time(from.last_active_time());
  } else {
    node.clear_last_active_time();
  }
  return merged.SerializeAsString() == local.SerializeAsString();
}

SidebarNode* RootFor(SpaceModel& model, int section) {
  return section == kSectionToday ? model.today_root() : model.pinned_root();
}

void CollectKeys(const SidebarNode& node, std::set<std::string>& keys) {
  for (const auto& child : node.children()) {
    keys.insert(Key(child->id()));
    CollectKeys(*child, keys);
  }
}

void CollectSplits(const SidebarNode& node, std::vector<base::Uuid>& splits) {
  for (const auto& child : node.children()) {
    if (child->type() == Type::kSplit) {
      splits.push_back(child->id());
    }
    CollectSplits(*child, splits);
  }
}


// Space themes (docs/design/sync.md, "Merged with main (space themes)").
// Each value is compared with what the entity already says, decoded, so an
// equivalent encoding (an older client's, or a stale `color_rgb`) isn't
// rewritten, and an applied remote value isn't committed back.

constexpr uint32_t kMaxRgb = 0xFFFFFF;
constexpr std::string_view kIconEmojiPrefix = "emoji:";
constexpr std::string_view kIconSymbolPrefix = "symbol:";
constexpr int kAppearanceAuto = 1;
constexpr int kAppearanceLight = 2;
constexpr int kAppearanceDark = 3;

// |color|'s 1-based index in kSpaceColorPalette, or 0 if it's off it.
int PaletteIndex(SpaceColor color) {
  for (size_t i = 0; i < kSpaceColorPalette.size(); ++i) {
    if (kSpaceColorPalette[i].color == color) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

std::optional<SpaceColor> FromPaletteIndex(int index) {
  if (index < 1 || index > static_cast<int>(kSpaceColorPalette.size())) {
    return std::nullopt;
  }
  return kSpaceColorPalette[index - 1].color;
}

// The index of the palette color closest to |rgb| (0xRRGGBB), grey aside:
// what clients that only know CompassSpace.color show for it.
int NearestPaletteIndex(uint32_t rgb) {
  const int red = (rgb >> 16) & 0xFF;
  const int green = (rgb >> 8) & 0xFF;
  const int blue = rgb & 0xFF;
  int best = 0;
  int best_distance = std::numeric_limits<int>::max();
  for (size_t i = 0; i < kSpaceColorPalette.size(); ++i) {
    const std::optional<SkColor> tint = kSpaceColorPalette[i].color.tint();
    if (!tint) {
      continue;
    }
    const int dr = static_cast<int>(SkColorGetR(*tint)) - red;
    const int dg = static_cast<int>(SkColorGetG(*tint)) - green;
    const int db = static_cast<int>(SkColorGetB(*tint)) - blue;
    const int distance = dr * dr + dg * dg + db * db;
    if (distance < best_distance) {
      best_distance = distance;
      best = static_cast<int>(i) + 1;
    }
  }
  return best;
}

SpaceColor FromRgb(uint32_t rgb) {
  return SpaceColor::FromSkColor(static_cast<SkColor>(rgb));
}

uint32_t ToRgb(SpaceColor color) {
  return color.tint().value_or(SK_ColorBLACK) & kMaxRgb;
}

// The space's color, or nullopt if the entity has none this version reads.
std::optional<SpaceColor> SpaceColorFromProto(
    const sync_pb::CompassSpace& space) {
  // `color_rgb` counts only while `color` still matches it: a client that
  // predates it may have picked a palette color since.
  if (space.has_color_rgb() && space.color_rgb() <= kMaxRgb &&
      space.color() == NearestPaletteIndex(space.color_rgb())) {
    return FromRgb(space.color_rgb());
  }
  return FromPaletteIndex(space.color());
}

std::optional<SpaceColor> ColorFromProto(const sync_pb::CompassColor& proto) {
  if (proto.has_rgb()) {
    if (proto.rgb() > kMaxRgb) {
      return std::nullopt;
    }
    return FromRgb(proto.rgb());
  }
  return FromPaletteIndex(proto.palette());
}

sync_pb::CompassColor ColorToProto(SpaceColor color) {
  sync_pb::CompassColor proto;
  if (const int index = PaletteIndex(color)) {
    proto.set_palette(index);
  } else {
    proto.set_rgb(ToRgb(color));
  }
  return proto;
}

// The gradient's colors this version reads; unreadable ones are left out.
std::vector<SpaceColor> GradientFromProto(const sync_pb::CompassSpace& space) {
  std::vector<SpaceColor> gradient;
  for (const sync_pb::CompassColor& proto : space.gradient()) {
    if (std::optional<SpaceColor> color = ColorFromProto(proto)) {
      gradient.push_back(*color);
    }
  }
  return gradient;
}

// nullopt for values this version doesn't know.
std::optional<SpaceAppearance> AppearanceFromProto(int value) {
  switch (value) {
    case 0:
    case kAppearanceAuto:
      return SpaceAppearance::kAuto;
    case kAppearanceLight:
      return SpaceAppearance::kLight;
    case kAppearanceDark:
      return SpaceAppearance::kDark;
    default:
      return std::nullopt;
  }
}

int AppearanceToProto(SpaceAppearance appearance) {
  switch (appearance) {
    case SpaceAppearance::kAuto:
      return kAppearanceAuto;
    case SpaceAppearance::kLight:
      return kAppearanceLight;
    case SpaceAppearance::kDark:
      return kAppearanceDark;
  }
  NOTREACHED();
}

// nullopt for an icon this version can't read (another kind, or invalid).
std::optional<SpaceIcon> IconFromProto(std::string_view value) {
  if (value.empty()) {
    return SpaceIcon();
  }
  if (base::StartsWith(value, kIconEmojiPrefix)) {
    return SpaceIcon::Emoji(value.substr(kIconEmojiPrefix.size()));
  }
  if (base::StartsWith(value, kIconSymbolPrefix)) {
    return SpaceIcon::Symbol(value.substr(kIconSymbolPrefix.size()));
  }
  return std::nullopt;
}

std::string IconToProto(const SpaceIcon& icon) {
  switch (icon.kind()) {
    case SpaceIcon::Kind::kNone:
      return std::string();
    case SpaceIcon::Kind::kEmoji:
      return std::string(kIconEmojiPrefix) + icon.value();
    case SpaceIcon::Kind::kSymbol:
      return std::string(kIconSymbolPrefix) + icon.value();
  }
  NOTREACHED();
}

// Writes |model|'s theme into |space|, leaving what already says the same.
void ThemeToProto(const SpaceModel& model, sync_pb::CompassSpace& space) {
  if (SpaceColorFromProto(space) != model.color()) {
    if (const int index = PaletteIndex(model.color())) {
      space.set_color(index);
      space.clear_color_rgb();
    } else {
      const uint32_t rgb = ToRgb(model.color());
      space.set_color(NearestPaletteIndex(rgb));
      space.set_color_rgb(rgb);
    }
  }

  bool gradient_same = space.gradient_size() ==
                       static_cast<int>(model.gradient().size());
  for (int i = 0; gradient_same && i < space.gradient_size(); ++i) {
    gradient_same = ColorFromProto(space.gradient(i)) ==
                    model.gradient()[static_cast<size_t>(i)];
  }
  if (!gradient_same) {
    space.clear_gradient();
    for (SpaceColor color : model.gradient()) {
      *space.add_gradient() = ColorToProto(color);
    }
  }

  // Defaults are left out.
  const double intensity =
      space.has_intensity() ? space.intensity() : kDefaultSpaceIntensity;
  if (intensity != model.intensity()) {
    if (model.intensity() == kDefaultSpaceIntensity) {
      space.clear_intensity();
    } else {
      space.set_intensity(model.intensity());
    }
  }
  const double grain = space.has_grain() ? space.grain() : 0;
  if (grain != model.grain()) {
    if (model.grain() == 0) {
      space.clear_grain();
    } else {
      space.set_grain(model.grain());
    }
  }

  // A value this version doesn't know was applied as auto, and stays until
  // the appearance changes here; the same for icons.
  const std::optional<SpaceAppearance> appearance =
      AppearanceFromProto(space.appearance());
  if (appearance.value_or(SpaceAppearance::kAuto) != model.appearance()) {
    if (model.appearance() == SpaceAppearance::kAuto) {
      space.clear_appearance();
    } else {
      space.set_appearance(AppearanceToProto(model.appearance()));
    }
  }
  const std::optional<SpaceIcon> icon = IconFromProto(space.icon());
  if (icon.value_or(SpaceIcon()) != model.icon()) {
    if (model.icon().is_none()) {
      space.clear_icon();
    } else {
      space.set_icon(IconToProto(model.icon()));
    }
  }
}

// Applies |space|'s theme to |model| through its setters, which clamp and
// validate. Absent fields are the defaults; unreadable values are ignored
// (a color, a gradient entry) or read as the default (appearance, icon).
void ThemeFromProto(const sync_pb::CompassSpace& space, SpaceModel& model) {
  if (std::optional<SpaceColor> color = SpaceColorFromProto(space)) {
    model.SetColor(*color);
  }
  // After the color: a grey space has no gradient.
  model.SetGradient(GradientFromProto(space));
  model.SetIntensity(space.has_intensity() ? space.intensity()
                                           : kDefaultSpaceIntensity);
  model.SetGrain(space.has_grain() ? space.grain() : 0);
  model.SetAppearance(
      AppearanceFromProto(space.appearance()).value_or(SpaceAppearance::kAuto));
  model.SetIcon(IconFromProto(space.icon()).value_or(SpaceIcon()));
}

}  // namespace

namespace internal {

std::vector<syncer::UniquePosition> AssignPositions(
    const std::vector<std::string>& keys,
    const std::vector<std::optional<syncer::UniquePosition>>& existing) {
  CHECK_EQ(keys.size(), existing.size());
  const size_t n = keys.size();
  // The longest run of existing positions already in increasing order.
  // Quadratic, but sibling lists are short.
  std::vector<size_t> length(n, 0);
  std::vector<std::optional<size_t>> previous(n);
  std::optional<size_t> best;
  for (size_t i = 0; i < n; ++i) {
    if (!existing[i] || !existing[i]->IsValid()) {
      continue;
    }
    length[i] = 1;
    for (size_t j = 0; j < i; ++j) {
      if (length[j] > 0 && existing[j]->LessThan(*existing[i]) &&
          length[j] + 1 > length[i]) {
        length[i] = length[j] + 1;
        previous[i] = j;
      }
    }
    if (!best || length[i] > length[*best]) {
      best = i;
    }
  }
  std::vector<bool> keep(n, false);
  for (std::optional<size_t> i = best; i; i = previous[*i]) {
    keep[*i] = true;
  }

  std::vector<syncer::UniquePosition> positions(n);
  for (size_t i = 0; i < n; ++i) {
    if (keep[i]) {
      positions[i] = *existing[i];
      continue;
    }
    std::optional<syncer::UniquePosition> next;
    for (size_t j = i + 1; j < n; ++j) {
      if (keep[j]) {
        next = *existing[j];
        break;
      }
    }
    const syncer::UniquePosition::Suffix suffix = SuffixFor(keys[i]);
    if (i > 0 && next) {
      positions[i] =
          syncer::UniquePosition::Between(positions[i - 1], *next, suffix);
    } else if (i > 0) {
      positions[i] = syncer::UniquePosition::After(positions[i - 1], suffix);
    } else if (next) {
      positions[i] = syncer::UniquePosition::Before(*next, suffix);
    } else {
      positions[i] = syncer::UniquePosition::InitialPosition(suffix);
    }
  }
  return positions;
}

}  // namespace internal

bool CompassSpacesSyncBridge::Delegate::IsShownInFocusedWindow(
    const SidebarNode& entry) {
  return false;
}

bool CompassSpacesSyncBridge::Delegate::IsInUse(const SidebarNode& entry) {
  return false;
}

DirectorySlot::DirectorySlot() = default;
DirectorySlot::DirectorySlot(std::string key,
                             std::optional<syncer::UniquePosition> position)
    : key(std::move(key)), position(std::move(position)) {}
DirectorySlot::DirectorySlot(const DirectorySlot&) = default;
DirectorySlot& DirectorySlot::operator=(const DirectorySlot&) = default;
DirectorySlot::~DirectorySlot() = default;

std::optional<std::vector<sync_pb::CompassProfile>>
CompassSpacesSyncBridge::Delegate::GetLocalProfiles() {
  return std::nullopt;
}

void CompassSpacesSyncBridge::Delegate::OnChainProfiles(
    const std::vector<sync_pb::CompassProfile>& profiles) {}

std::string CompassSpacesSyncBridge::Delegate::GetProfileUuid() {
  return std::string();
}

SpaceModel* CompassSpacesSyncBridge::Delegate::AdoptSpace(
    const base::Uuid& id) {
  return nullptr;
}

std::vector<DirectorySlot>
CompassSpacesSyncBridge::Delegate::GetDirectoryOrder() {
  return {};
}

void CompassSpacesSyncBridge::Delegate::OnSpacePositions(
    const std::map<std::string, syncer::UniquePosition>& positions,
    bool remote) {}

CompassSpacesSyncBridge::CompassSpacesSyncBridge(
    std::unique_ptr<syncer::DataTypeLocalChangeProcessor> change_processor,
    syncer::OnceDataTypeStoreFactory create_store,
    SpaceList* list,
    Delegate* delegate)
    : syncer::DataTypeSyncBridge(std::move(change_processor)),
      list_(list),
      delegate_(delegate) {
  CHECK(list_);
  list_->AddObserver(this);
  Observe(list_->favorites());
  for (const auto& space : list_->spaces()) {
    Observe(*space);
  }
  std::move(create_store)
      .Run(syncer::COMPASS_SPACES,
           base::BindOnce(&CompassSpacesSyncBridge::OnStoreCreated,
                          weak_factory_.GetWeakPtr()));
}

CompassSpacesSyncBridge::~CompassSpacesSyncBridge() {
  for (const auto& space : list_->spaces()) {
    space->RemoveObserver(this);
  }
  list_->favorites().RemoveObserver(this);
  list_->RemoveObserver(this);
}

void CompassSpacesSyncBridge::OnModelLoaded() {
  model_loaded_ = true;
  MaybeStart();
}

void CompassSpacesSyncBridge::NotifyLocalChange() {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnStoreCreated(
    const std::optional<syncer::ModelError>& error,
    std::unique_ptr<syncer::DataTypeStore> store) {
  if (error) {
    change_processor()->ReportError(*error);
    return;
  }
  store_ = std::move(store);
  store_->ReadAllDataAndMetadata(
      base::BindOnce(&CompassSpacesSyncBridge::OnReadAllDataAndMetadata,
                     weak_factory_.GetWeakPtr()));
}

void CompassSpacesSyncBridge::OnReadAllDataAndMetadata(
    const std::optional<syncer::ModelError>& error,
    std::unique_ptr<syncer::DataTypeStore::RecordList> records,
    std::unique_ptr<syncer::MetadataBatch> metadata) {
  if (error) {
    change_processor()->ReportError(*error);
    return;
  }
  for (const syncer::DataTypeStore::Record& record : *records) {
    sync_pb::CompassSpacesSpecifics specifics;
    // A record that doesn't parse is dropped; the next pass puts it back if
    // the model has it.
    if (specifics.ParseFromString(record.value)) {
      mirror_[record.id] = std::move(specifics);
    }
  }
  pending_metadata_ = std::move(metadata);
  store_loaded_ = true;
  MaybeStart();
}

void CompassSpacesSyncBridge::MaybeStart() {
  if (ready_ || !store_loaded_ || !model_loaded_) {
    return;
  }
  ready_ = true;
  VLOG(1) << "[compass-sync] spaces: model ready, " << mirror_.size()
          << " synced entities, " << list_->spaces().size() << " spaces";
  change_processor()->ModelReadyToSync(std::move(pending_metadata_));
  if (delegate_) {
    // What the last run had: the switcher's order and the profile list
    // don't wait for the server.
    ReportSpacePositions(/*remote=*/false);
    std::vector<sync_pb::CompassProfile> profiles = MirrorProfiles();
    if (!profiles.empty()) {
      delegate_->OnChainProfiles(profiles);
    }
  }
  // Changes made while sync wasn't running.
  ScheduleSendLocalChanges();
  if (delegate_ && change_processor()->IsTrackingMetadata()) {
    delegate_->OnFaviconsWritable();
  }
}

void CompassSpacesSyncBridge::Commit(
    std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch) {
  store_->CommitWriteBatch(
      std::move(batch), base::BindOnce(&CompassSpacesSyncBridge::OnCommitted,
                                       weak_factory_.GetWeakPtr()));
}

void CompassSpacesSyncBridge::OnCommitted(
    const std::optional<syncer::ModelError>& error) {
  if (error) {
    change_processor()->ReportError(*error);
  }
}

// Local changes ---------------------------------------------------------------

void CompassSpacesSyncBridge::ScheduleSendLocalChanges() {
  if (send_scheduled_ || !ready_) {
    return;
  }
  send_scheduled_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&CompassSpacesSyncBridge::SendLocalChanges,
                                weak_factory_.GetWeakPtr()));
}

void CompassSpacesSyncBridge::SendLocalChanges() {
  send_scheduled_ = false;
  if (!ready_ || !store_ || !change_processor()->IsTrackingMetadata()) {
    return;
  }
  std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();
  if (SendLocalChangesTo(*batch) > 0) {
    Commit(std::move(batch));
  }
}

size_t CompassSpacesSyncBridge::SendLocalChangesTo(
    syncer::DataTypeStore::WriteBatch& batch) {
  Mirror local = ComputeLocalSpecifics();
  // The home profile's bridge also has the chain's profiles (slice 5).
  const std::optional<std::vector<sync_pb::CompassProfile>> profiles =
      delegate_ ? delegate_->GetLocalProfiles() : std::nullopt;
  if (profiles) {
    AddLocalProfiles(*profiles, local);
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  std::optional<base::TimeTicks> next_send;
  throttled_.clear();
  size_t puts = 0;
  for (const auto& [key, specifics] : local) {
    std::string serialized = specifics.SerializeAsString();
    auto it = mirror_.find(key);
    if (it != mirror_.end() && it->second.SerializeAsString() == serialized) {
      continue;
    }
    // Title, URL and last-active churn waits until the entity's last commit
    // is kContentCommitInterval old; anything else goes now, with it.
    if (it != mirror_.end() && OnlyContentDiffers(specifics, it->second)) {
      auto last = last_commit_.find(key);
      if (last != last_commit_.end() &&
          now - last->second < kContentCommitInterval) {
        const base::TimeTicks due = last->second + kContentCommitInterval;
        next_send = next_send ? std::min(*next_send, due) : due;
        throttled_.insert(key);
        continue;
      }
    }
    last_commit_[key] = now;
    auto data = std::make_unique<syncer::EntityData>();
    // The name isn't needed, and goes to the server; the ID is enough.
    data->name = key;
    *data->specifics.mutable_compass_spaces() = specifics;
    change_processor()->Put(key, std::move(data),
                            batch.GetMetadataChangeList());
    batch.WriteData(key, serialized);
    mirror_[key] = specifics;
    ++puts;
  }

  std::vector<std::string> gone;
  std::optional<std::set<std::string>> entry_hosts;
  for (const auto& [key, specifics] : mirror_) {
    if (local.contains(key)) {
      continue;
    }
    if (specifics.has_favicon()) {
      // Not in the model: only garbage goes.
      if (!entry_hosts) {
        entry_hosts = GetEntryHosts();
      }
      if (IsFaviconGarbage(specifics.favicon(), *entry_hosts)) {
        gone.push_back(key);
      }
      continue;
    }
    if (specifics.has_profile()) {
      // Profiles aren't deleted through sync (deleting one stays on its
      // device), but a home entity this device's home no longer has is one
      // it gave up for the chain's (two devices that each started as home).
      if (profiles && specifics.profile().home()) {
        gone.push_back(key);
      }
      continue;
    }
    if (WasDeletedLocally(specifics, local)) {
      gone.push_back(key);
    }
  }
  for (const std::string& key : gone) {
    change_processor()->Delete(key, syncer::DeletionOrigin::Unspecified(),
                               batch.GetMetadataChangeList());
    batch.DeleteData(key);
    mirror_.erase(key);
    last_commit_.erase(key);
  }
  std::erase_if(unparked_, [this](const std::string& key) {
    auto it = mirror_.find(key);
    return it == mirror_.end() || !it->second.has_node() ||
           it->second.node().parent_uuid().empty();
  });

  if (puts > 0 || !gone.empty()) {
    VLOG(1) << "[compass-sync] spaces: committing " << puts << " changed, "
            << gone.size() << " deleted, " << throttled_.size()
            << " waiting (content)";
  }
  ReportSpacePositions(/*remote=*/false);
  if (next_send) {
    ScheduleThrottledSend(*next_send);
  }
  ScheduleArchiveExpiry();
  return puts + gone.size();
}

void CompassSpacesSyncBridge::ScheduleThrottledSend(base::TimeTicks when) {
  if (throttle_timer_.IsRunning() &&
      throttle_timer_.desired_run_time() <= when) {
    return;
  }
  const base::TimeDelta delay = when - base::TimeTicks::Now();
  VLOG(1) << "[compass-sync] spaces: content of " << throttled_.size()
          << " entities waits " << delay.InMilliseconds() << " ms";
  throttle_timer_.Start(FROM_HERE, delay, this,
                        &CompassSpacesSyncBridge::SendLocalChanges);
}

void CompassSpacesSyncBridge::ScheduleArchiveExpiry() {
  const base::Time now = base::Time::Now();
  std::optional<base::Time> next;
  for (const auto& space : list_->spaces()) {
    for (const ArchivedEntry& entry : space->archive()) {
      const base::Time expiry = entry.archived_at + kArchiveSyncPeriod;
      if (expiry > now && (!next || expiry < *next)) {
        next = expiry;
      }
    }
  }
  if (!next) {
    archive_expiry_timer_.Stop();
    return;
  }
  // A moment after, so the entry is past the period when the pass runs.
  archive_expiry_timer_.Start(FROM_HERE, *next - now + base::Seconds(1), this,
                              &CompassSpacesSyncBridge::SendLocalChanges);
}

CompassSpacesSyncBridge::Mirror
CompassSpacesSyncBridge::ComputeLocalSpecifics() const {
  Mirror local;

  // Spaces, in the list's order (the profile's order in the switcher; see
  // compass_spaces_sync.cc).
  std::vector<std::string> keys;
  std::vector<std::optional<syncer::UniquePosition>> existing;
  std::vector<const SpaceModel*> spaces;
  for (const auto& space : list_->spaces()) {
    const std::string key = Key(space->id());
    if (std::ranges::contains(keys, key)) {
      continue;
    }
    std::optional<syncer::UniquePosition> position;
    auto it = mirror_.find(key);
    if (it != mirror_.end() && it->second.has_space() &&
        it->second.space().has_directory_position()) {
      position = ValidPosition(it->second.space().directory_position());
    }
    keys.push_back(key);
    existing.push_back(position);
    spaces.push_back(space.get());
  }
  const std::vector<syncer::UniquePosition> positions =
      AssignSpacePositions(keys, existing);
  const std::string profile_uuid =
      delegate_ ? delegate_->GetProfileUuid() : std::string();
  for (size_t i = 0; i < spaces.size(); ++i) {
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror_.find(keys[i]);
    if (it != mirror_.end() && it->second.has_space()) {
      // Keeps what this version doesn't know (fields from later versions).
      specifics = it->second;
    }
    sync_pb::CompassSpace& space = *specifics.mutable_space();
    const SpaceModel& model = *spaces[i];
    COMPASS_SET_STRING(space, uuid, keys[i]);
    COMPASS_SET_STRING(space, name, base::UTF16ToUTF8(model.name()));
    ThemeToProto(model, space);
    const int64_t archive_after = model.archive_after().InSeconds();
    if (!space.has_archive_after_seconds() ||
        space.archive_after_seconds() != archive_after) {
      space.set_archive_after_seconds(archive_after);
    }
    if (!existing[i] || !existing[i]->Equals(positions[i])) {
      *space.mutable_directory_position() = positions[i].ToProto();
    }
    if (!profile_uuid.empty()) {
      COMPASS_SET_STRING(space, profile_uuid, profile_uuid);
    }
    local[keys[i]] = std::move(specifics);
  }

  // Nodes: each space's pinned tree and Today, then the favorites, then the
  // archives.
  std::set<std::string> model_keys;
  for (const auto& space : list_->spaces()) {
    CollectKeys(*space->pinned_root(), model_keys);
    CollectKeys(*space->today_root(), model_keys);
  }
  CollectKeys(*list_->favorites().pinned_root(), model_keys);
  for (const SpaceModel* space : spaces) {
    AddLocalNodes(*space->pinned_root(), std::string(), kSectionPinned,
                  Key(space->id()), model_keys, local);
    AddLocalNodes(*space->today_root(), std::string(), kSectionToday,
                  Key(space->id()), model_keys, local);
  }
  AddLocalNodes(*list_->favorites().pinned_root(), std::string(),
                kSectionFavorites, std::string(), model_keys, local);
  for (const SpaceModel* space : spaces) {
    AddLocalArchive(*space, local);
  }
  return local;
}

std::vector<syncer::UniquePosition>
CompassSpacesSyncBridge::AssignSpacePositions(
    const std::vector<std::string>& keys,
    const std::vector<std::optional<syncer::UniquePosition>>& existing) const {
  const std::vector<DirectorySlot> order =
      delegate_ ? delegate_->GetDirectoryOrder() : std::vector<DirectorySlot>();
  // Every profile's spaces, this one's with the positions of its mirror,
  // the others' with what their bridges synced: positions given here fall
  // between the other accounts' (docs/design/sync.md, "Profiles").
  std::vector<std::string> all_keys;
  std::vector<std::optional<syncer::UniquePosition>> all_existing;
  std::vector<size_t> own;
  for (const DirectorySlot& slot : order) {
    auto it = std::ranges::find(keys, slot.key);
    if (it == keys.end()) {
      if (!std::ranges::contains(all_keys, slot.key)) {
        all_keys.push_back(slot.key);
        all_existing.push_back(slot.position);
      }
      continue;
    }
    // This profile's spaces must come in the list's order, each once
    // (compass_spaces_sync.cc keeps the two orders equal); else the order is
    // being changed and only this profile's spaces count.
    if (static_cast<size_t>(it - keys.begin()) != own.size()) {
      own.clear();
      break;
    }
    all_existing.push_back(existing[own.size()]);
    own.push_back(all_keys.size());
    all_keys.push_back(slot.key);
  }
  if (keys.empty() || own.size() != keys.size()) {
    return internal::AssignPositions(keys, existing);
  }
  const std::vector<syncer::UniquePosition> all =
      internal::AssignPositions(all_keys, all_existing);
  std::vector<syncer::UniquePosition> positions;
  for (size_t index : own) {
    positions.push_back(all[index]);
  }
  return positions;
}

void CompassSpacesSyncBridge::AddLocalProfiles(
    const std::vector<sync_pb::CompassProfile>& profiles,
    Mirror& out) const {
  for (const sync_pb::CompassProfile& profile : profiles) {
    const std::string& key = profile.uuid();
    if (!ParseKey(key).is_valid() || out.contains(key)) {
      continue;
    }
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror_.find(key);
    if (it != mirror_.end() && it->second.has_profile()) {
      // Keeps what this version doesn't know.
      specifics = it->second;
    }
    sync_pb::CompassProfile& entity = *specifics.mutable_profile();
    COMPASS_SET_STRING(entity, uuid, key);
    COMPASS_SET_STRING(entity, name, profile.name());
    if (entity.avatar() != profile.avatar()) {
      if (profile.avatar() > 0) {
        entity.set_avatar(profile.avatar());
      } else {
        entity.clear_avatar();
      }
    }
    if (entity.home() != profile.home()) {
      if (profile.home()) {
        entity.set_home(true);
      } else {
        entity.clear_home();
      }
    }
    out[key] = std::move(specifics);
  }
}

std::vector<sync_pb::CompassProfile> CompassSpacesSyncBridge::MirrorProfiles()
    const {
  std::vector<sync_pb::CompassProfile> profiles;
  for (const auto& [key, specifics] : mirror_) {
    if (specifics.has_profile()) {
      profiles.push_back(specifics.profile());
    }
  }
  return profiles;
}

void CompassSpacesSyncBridge::ReportSpacePositions(bool remote) {
  if (!delegate_) {
    return;
  }
  std::map<std::string, syncer::UniquePosition> positions;
  for (const auto& space : list_->spaces()) {
    const std::string key = Key(space->id());
    auto it = mirror_.find(key);
    if (it == mirror_.end() || !it->second.has_space() ||
        !it->second.space().has_directory_position()) {
      continue;
    }
    if (std::optional<syncer::UniquePosition> position =
            ValidPosition(it->second.space().directory_position())) {
      positions.emplace(key, *position);
    }
  }
  delegate_->OnSpacePositions(positions, remote);
}

void CompassSpacesSyncBridge::AddLocalArchive(const SpaceModel& space,
                                              Mirror& out) const {
  const std::string space_key = Key(space.id());
  const base::Time cutoff = base::Time::Now() - kArchiveSyncPeriod;
  for (const ArchivedEntry& entry : space.archive()) {
    const std::string key = Key(entry.id);
    if (entry.archived_at < cutoff || !entry.id.is_valid() ||
        entry.tabs.empty() || out.contains(key)) {
      // Too old for the server (kept here), or not a usable ID.
      continue;
    }
    const bool single =
        entry.tabs.size() == 1 && entry.tabs[0].id == entry.id;
    if (!single && std::ranges::any_of(entry.tabs, [&out](const auto& tab) {
          return !tab.id.is_valid() || out.contains(Key(tab.id));
        })) {
      continue;
    }
    const int64_t archived_time = ToProtoTime(entry.archived_at);

    // The entry. Its position and last-active time stay what they were in
    // Today: the archive is ordered by time.
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror_.find(key);
    if (it != mirror_.end() && it->second.has_node()) {
      specifics = it->second;
    }
    sync_pb::CompassNode& node = *specifics.mutable_node();
    COMPASS_SET_STRING(node, uuid, key);
    COMPASS_SET_STRING(node, space_uuid, space_key);
    COMPASS_SET_STRING(node, parent_uuid, std::string());
    if (node.section() != kSectionArchive) {
      node.set_section(kSectionArchive);
    }
    const int type = single ? kNodeTab : kNodeSplit;
    if (node.type() != type) {
      node.set_type(type);
    }
    COMPASS_SET_STRING(node, url,
                       single ? entry.tabs[0].url.possibly_invalid_spec()
                              : std::string());
    COMPASS_SET_STRING(node, title,
                       single ? base::UTF16ToUTF8(entry.tabs[0].title)
                              : std::string());
    COMPASS_SET_STRING(node, custom_title, std::string());
    if (node.collapsed()) {
      node.clear_collapsed();
    }
    if (node.archived_time() != archived_time) {
      node.set_archived_time(archived_time);
    }
    out[key] = std::move(specifics);
    if (single) {
      continue;
    }

    // A split's panes, in order.
    std::vector<std::string> keys;
    std::vector<std::optional<syncer::UniquePosition>> existing;
    for (const ArchivedEntry::Tab& tab : entry.tabs) {
      const std::string pane_key = Key(tab.id);
      std::optional<syncer::UniquePosition> position;
      auto pane_it = mirror_.find(pane_key);
      if (pane_it != mirror_.end() && pane_it->second.has_node() &&
          pane_it->second.node().section() == kSectionArchive &&
          pane_it->second.node().parent_uuid() == key &&
          pane_it->second.node().has_position()) {
        position = ValidPosition(pane_it->second.node().position());
      }
      keys.push_back(pane_key);
      existing.push_back(position);
    }
    const std::vector<syncer::UniquePosition> positions =
        internal::AssignPositions(keys, existing);
    for (size_t i = 0; i < entry.tabs.size(); ++i) {
      sync_pb::CompassSpacesSpecifics pane_specifics;
      auto pane_it = mirror_.find(keys[i]);
      if (pane_it != mirror_.end() && pane_it->second.has_node()) {
        pane_specifics = pane_it->second;
      }
      sync_pb::CompassNode& pane = *pane_specifics.mutable_node();
      COMPASS_SET_STRING(pane, uuid, keys[i]);
      COMPASS_SET_STRING(pane, space_uuid, space_key);
      COMPASS_SET_STRING(pane, parent_uuid, key);
      if (pane.section() != kSectionArchive) {
        pane.set_section(kSectionArchive);
      }
      if (pane.type() != kNodeTab) {
        pane.set_type(kNodeTab);
      }
      COMPASS_SET_STRING(pane, url, entry.tabs[i].url.possibly_invalid_spec());
      COMPASS_SET_STRING(pane, title, base::UTF16ToUTF8(entry.tabs[i].title));
      COMPASS_SET_STRING(pane, custom_title, std::string());
      if (pane.archived_time() != archived_time) {
        pane.set_archived_time(archived_time);
      }
      if (!existing[i] || !existing[i]->Equals(positions[i])) {
        *pane.mutable_position() = positions[i].ToProto();
      }
      out[keys[i]] = std::move(pane_specifics);
    }
  }
}

bool CompassSpacesSyncBridge::IsSyncableToday(const SidebarNode& node) const {
  if (mirror_.contains(Key(node.id()))) {
    // Synced once, synced for good (a page that went blank, say).
    return true;
  }
  switch (node.type()) {
    case Type::kTab:
      return IsRestorableURL(node.url());
    case Type::kSplit:
      return !node.children().empty() &&
             std::ranges::all_of(node.children(), [this](const auto& pane) {
               return IsSyncableToday(*pane);
             });
    case Type::kFolder:
    case Type::kRoot:
      return true;
  }
  return true;
}

void CompassSpacesSyncBridge::AddLocalNodes(
    const SidebarNode& parent,
    const std::string& parent_key,
    int section,
    const std::string& space_key,
    const std::set<std::string>& model_keys,
    Mirror& out) const {
  const bool at_root = parent.type() == Type::kRoot;
  std::vector<const SidebarNode*> included;
  std::vector<const SidebarNode*> placed;
  std::vector<std::string> keys;
  std::vector<std::optional<syncer::UniquePosition>> existing;
  for (const auto& child : parent.children()) {
    const std::string key = Key(child->id());
    if (out.contains(key)) {
      // Another node has this ID (a damaged file): only the first syncs.
      continue;
    }
    if (section == kSectionToday && !IsSyncableToday(*child)) {
      // A new tab or blank page stays here.
      continue;
    }
    auto it = mirror_.find(key);
    const sync_pb::CompassNode* synced =
        it != mirror_.end() && it->second.has_node() ? &it->second.node()
                                                     : nullptr;
    const bool same_place = synced && synced->section() == section &&
                            synced->space_uuid() == space_key;
    if (at_root && same_place && !synced->parent_uuid().empty() &&
        !model_keys.contains(synced->parent_uuid()) &&
        !unparked_.contains(key)) {
      // Parked: it waits here for its parent, and keeps the place the
      // server has for it.
      sync_pb::CompassSpacesSpecifics specifics = it->second;
      FillNodeContent(*child, *specifics.mutable_node());
      if (section == kSectionToday) {
        FillLastActive(*child, *specifics.mutable_node());
      }
      out[key] = std::move(specifics);
      included.push_back(child.get());
      continue;
    }
    std::optional<syncer::UniquePosition> position;
    if (same_place && synced->parent_uuid() == parent_key &&
        synced->has_position()) {
      position = ValidPosition(synced->position());
    }
    // Reserves the key against duplicates further down.
    out[key];
    included.push_back(child.get());
    placed.push_back(child.get());
    keys.push_back(key);
    existing.push_back(position);
  }

  const std::vector<syncer::UniquePosition> positions =
      internal::AssignPositions(keys, existing);
  for (size_t i = 0; i < placed.size(); ++i) {
    sync_pb::CompassSpacesSpecifics specifics;
    auto it = mirror_.find(keys[i]);
    if (it != mirror_.end() && it->second.has_node()) {
      // Keeps what this version doesn't know.
      specifics = it->second;
    }
    sync_pb::CompassNode& node = *specifics.mutable_node();
    COMPASS_SET_STRING(node, uuid, keys[i]);
    COMPASS_SET_STRING(node, space_uuid, space_key);
    COMPASS_SET_STRING(node, parent_uuid, parent_key);
    if (node.section() != section) {
      node.set_section(section);
    }
    const int type = TypeToProto(placed[i]->type());
    if (node.type() != type) {
      node.set_type(type);
    }
    FillNodeContent(*placed[i], node);
    if (section == kSectionToday) {
      FillLastActive(*placed[i], node);
    }
    if (node.has_archived_time()) {
      // Back from the archive.
      node.clear_archived_time();
    }
    if (!existing[i] || !existing[i]->Equals(positions[i])) {
      *node.mutable_position() = positions[i].ToProto();
    }
    out[keys[i]] = std::move(specifics);
  }

  for (const SidebarNode* child : included) {
    if (!child->children().empty()) {
      AddLocalNodes(*child, Key(child->id()), section, space_key, model_keys,
                    out);
    }
  }
}

bool CompassSpacesSyncBridge::WasDeletedLocally(
    const sync_pb::CompassSpacesSpecifics& specifics,
    const Mirror& local) const {
  if (specifics.has_profile() || specifics.has_favicon()) {
    // SendLocalChangesTo() decides.
    return false;
  }
  if (specifics.has_space()) {
    return true;
  }
  if (!specifics.has_node() || !TypeFromProto(specifics.node().type())) {
    return false;
  }
  const sync_pb::CompassNode& node = specifics.node();
  switch (node.section()) {
    case kSectionArchive:
      if (!IsArchiveComplete(node)) {
        // Waits for the rest of its entry.
        return false;
      }
      [[fallthrough]];
    case kSectionPinned:
    case kSectionToday: {
      // Its space is here, or was and goes too. A space this device never
      // had (another profile's, later) means it was never applied.
      if (local.contains(node.space_uuid())) {
        return true;
      }
      auto it = mirror_.find(node.space_uuid());
      return it != mirror_.end() && it->second.has_space();
    }
    case kSectionFavorites:
      return node.type() == kNodeTab;
    default:
      return false;
  }
}

bool CompassSpacesSyncBridge::IsArchiveComplete(
    const sync_pb::CompassNode& node) const {
  if (!node.parent_uuid().empty()) {
    // A pane: its split is in the archive too.
    auto it = mirror_.find(node.parent_uuid());
    return it != mirror_.end() && it->second.has_node() &&
           it->second.node().section() == kSectionArchive;
  }
  if (node.type() != kNodeSplit) {
    return true;
  }
  return std::ranges::any_of(mirror_, [&node](const auto& entry) {
    return entry.second.has_node() &&
           entry.second.node().section() == kSectionArchive &&
           entry.second.node().parent_uuid() == node.uuid();
  });
}

// Favicons --------------------------------------------------------------------

std::map<std::string, std::vector<GURL>>
CompassSpacesSyncBridge::GetEntryUrlsByHost() const {
  std::map<std::string, std::vector<GURL>> urls;
  auto add = [&urls](const GURL& url) {
    if (std::string host = HostOf(url); !host.empty()) {
      std::vector<GURL>& host_urls = urls[std::move(host)];
      if (!std::ranges::contains(host_urls, url)) {
        host_urls.push_back(url);
      }
    }
  };
  std::function<void(const SidebarNode&)> walk = [&](const SidebarNode& node) {
    if (node.type() == Type::kTab) {
      add(node.url());
    }
    for (const auto& child : node.children()) {
      walk(*child);
    }
  };
  for (const auto& space : list_->spaces()) {
    walk(*space->pinned_root());
    walk(*space->today_root());
    for (const ArchivedEntry& entry : space->archive()) {
      for (const ArchivedEntry::Tab& tab : entry.tabs) {
        add(tab.url);
      }
    }
  }
  walk(*list_->favorites().pinned_root());
  return urls;
}

std::set<std::string> CompassSpacesSyncBridge::GetEntryHosts() const {
  std::set<std::string> hosts;
  for (const auto& [host, urls] : GetEntryUrlsByHost()) {
    hosts.insert(host);
  }
  return hosts;
}

bool CompassSpacesSyncBridge::OfferFavicon(const std::string& host,
                                           const std::string& png,
                                           int source_size) {
  const std::string lower_host = base::ToLowerASCII(host);
  if (!ready_ || !store_ || !change_processor()->IsTrackingMetadata() ||
      lower_host.empty() || png.empty() || png.size() > kFaviconMaxBytes ||
      source_size <= 0 || !GetEntryHosts().contains(lower_host)) {
    return false;
  }
  const std::string key = FaviconKey(lower_host);
  const base::Time now = base::Time::Now();
  auto it = mirror_.find(key);
  if (it != mirror_.end() && it->second.has_favicon()) {
    const sync_pb::CompassFavicon& old = it->second.favicon();
    const bool larger = source_size > old.source_size();
    // Two devices with different icons of one size don't take turns.
    const bool refresh = source_size == old.source_size() &&
                         png != old.png() &&
                         now - FromWindowsMicros(old.update_time()) >=
                             kFaviconRefreshAge;
    if (!larger && !refresh) {
      return false;
    }
  }
  sync_pb::CompassSpacesSpecifics specifics;
  sync_pb::CompassFavicon* favicon = specifics.mutable_favicon();
  favicon->set_uuid(key);
  favicon->set_host(lower_host);
  favicon->set_png(png);
  favicon->set_source_size(source_size);
  favicon->set_update_time(ToWindowsMicros(now));

  std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();
  auto data = std::make_unique<syncer::EntityData>();
  data->name = key;
  *data->specifics.mutable_compass_spaces() = specifics;
  change_processor()->Put(key, std::move(data),
                          batch->GetMetadataChangeList());
  batch->WriteData(key, specifics.SerializeAsString());
  mirror_[key] = std::move(specifics);
  last_commit_[key] = base::TimeTicks::Now();
  Commit(std::move(batch));
  VLOG(1) << "[compass-sync] spaces: favicon of " << lower_host << " ("
          << source_size << " px source, " << png.size() << " bytes)";
  return true;
}

bool CompassSpacesSyncBridge::IsFaviconGarbage(
    const sync_pb::CompassFavicon& favicon,
    const std::set<std::string>& hosts) const {
  return !hosts.contains(base::ToLowerASCII(favicon.host())) &&
         base::Time::Now() - FromWindowsMicros(favicon.update_time()) >
             kFaviconGracePeriod;
}

void CompassSpacesSyncBridge::ApplyMirrorFavicons(
    const std::optional<std::set<std::string>>& hosts) {
  if (!delegate_ || (hosts && hosts->empty())) {
    return;
  }
  std::vector<sync_pb::CompassFavicon> favicons;
  for (const auto& [key, specifics] : mirror_) {
    if (specifics.has_favicon() && !specifics.favicon().png().empty() &&
        (!hosts ||
         hosts->contains(base::ToLowerASCII(specifics.favicon().host())))) {
      favicons.push_back(specifics.favicon());
    }
  }
  if (!favicons.empty()) {
    delegate_->ApplyFavicons(favicons);
  }
}

// Remote changes --------------------------------------------------------------

std::unique_ptr<syncer::MetadataChangeList>
CompassSpacesSyncBridge::CreateMetadataChangeList() {
  return syncer::DataTypeStore::WriteBatch::CreateMetadataChangeList();
}

std::optional<syncer::ModelError> CompassSpacesSyncBridge::MergeFullSyncData(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_data) {
  return ApplyRemote(std::move(metadata_change_list), std::move(entity_data),
                     /*initial_merge=*/true);
}

std::optional<syncer::ModelError>
CompassSpacesSyncBridge::ApplyIncrementalSyncChanges(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_changes) {
  return ApplyRemote(std::move(metadata_change_list),
                     std::move(entity_changes), /*initial_merge=*/false);
}

std::optional<syncer::ModelError> CompassSpacesSyncBridge::ApplyRemote(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList changes,
    bool initial_merge) {
  CHECK(store_);
  std::unique_ptr<syncer::DataTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch(std::move(metadata_change_list));

  // The mirror first: what the model then does with them matches it, so it
  // isn't committed back.
  std::vector<std::string> spaces;
  std::vector<std::string> nodes;
  std::set<std::string> deleted;
  bool profiles_changed = false;
  // Hosts whose icons or entries came: their icons go to the delegate.
  std::set<std::string> favicon_hosts;
  for (const std::unique_ptr<syncer::EntityChange>& change : changes) {
    const std::string& key = change->storage_key();
    if (change->type() == syncer::EntityChange::ACTION_DELETE) {
      auto it = mirror_.find(key);
      if (it != mirror_.end() && it->second.has_profile()) {
        profiles_changed = true;
      }
      mirror_.erase(key);
      unparked_.erase(key);
      batch->DeleteData(key);
      deleted.insert(key);
      continue;
    }
    const sync_pb::CompassSpacesSpecifics& specifics =
        change->data().specifics.compass_spaces();
    mirror_[key] = specifics;
    batch->WriteData(key, specifics.SerializeAsString());
    if (specifics.has_profile()) {
      profiles_changed = true;
      continue;
    }
    if (specifics.has_favicon()) {
      favicon_hosts.insert(base::ToLowerASCII(specifics.favicon().host()));
      continue;
    }
    if (specifics.has_node() && specifics.node().has_url()) {
      favicon_hosts.insert(HostOf(GURL(specifics.node().url())));
    }
    (specifics.has_space() ? spaces : nodes).push_back(key);
  }

  {
    base::AutoReset<bool> applying(&applying_remote_, true);
    // Another device's moves can empty a Today folder for a moment; it
    // closes there, and its deletion comes too.
    SpaceModel::KeepEmptyFolders keep_empty_folders;
    if (profiles_changed && delegate_) {
      // First: the home profile may take the chain's UUID, which the pass
      // below then uses.
      delegate_->OnChainProfiles(MirrorProfiles());
    }
    const std::set<std::string> created = ApplySpaces(spaces, deleted);
    // Nodes that waited for a space that just came.
    std::set<std::string> listed(nodes.begin(), nodes.end());
    for (const auto& [key, specifics] : mirror_) {
      if (specifics.has_node() &&
          created.contains(specifics.node().space_uuid()) &&
          !listed.contains(key)) {
        nodes.push_back(key);
        listed.insert(key);
      }
    }
    if (initial_merge) {
      MergeWithChain(nodes, std::set<std::string>(spaces.begin(),
                                                  spaces.end()));
    }
    ApplyNodes(nodes, deleted);
    if (initial_merge) {
      DedupeToday(nodes);
    }
  }
  VLOG(1) << "[compass-sync] spaces: applied " << changes.size()
          << (initial_merge ? " entities (initial merge)" : " remote changes")
          << ", " << list_->spaces().size() << " spaces";

  if (!spaces.empty() || initial_merge) {
    // The switcher places the spaces among other profiles' by position
    // (slice 5), before positions are computed from it again below.
    ReportSpacePositions(/*remote=*/true);
  }
  if (initial_merge) {
    // This device's own spaces and entries join the chain's.
    SendLocalChangesTo(*batch);
  }
  Commit(std::move(batch));
  if (initial_merge && delegate_) {
    delegate_->OnFaviconsWritable();
  }
  ApplyMirrorFavicons(initial_merge
                          ? std::nullopt
                          : std::optional<std::set<std::string>>(
                                std::move(favicon_hosts)));
  if (!initial_merge) {
    // Fix-ups the model made (cycles, invalid placements), after this
    // update is done.
    ScheduleSendLocalChanges();
  }
  return std::nullopt;
}

std::set<std::string> CompassSpacesSyncBridge::ApplySpaces(
    const std::vector<std::string>& keys,
    const std::set<std::string>& deleted) {
  std::set<std::string> created;
  for (const std::string& key : keys) {
    const sync_pb::CompassSpace& remote = mirror_[key].space();
    const base::Uuid id = ParseKey(key);
    if (!id.is_valid()) {
      continue;
    }
    SpaceModel* space = list_->FindSpace(id);
    if (!space) {
      if (FindNode(id)) {
        continue;
      }
      // Moved here from another profile on another device: if this device
      // has it in that profile, it comes over with its entries (slice 5).
      space = delegate_ ? delegate_->AdoptSpace(id) : nullptr;
      if (space && !remote.name().empty()) {
        space->SetName(base::UTF8ToUTF16(remote.name()));
      }
      if (!space) {
        space = list_->AddSpace(list_->spaces().size(),
                                base::UTF8ToUTF16(remote.name()), id);
      }
      Observe(*space);
      created.insert(key);
    } else if (!remote.name().empty()) {
      space->SetName(base::UTF8ToUTF16(remote.name()));
    }
    ThemeFromProto(remote, *space);
    if (remote.has_archive_after_seconds() &&
        remote.archive_after_seconds() >= 0) {
      space->SetArchiveAfter(base::Seconds(remote.archive_after_seconds()));
    }
  }
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    if (SpaceModel* space = id.is_valid() ? list_->FindSpace(id) : nullptr) {
      // The last space stays (and the next pass puts it back on the server).
      DeleteSpace(*space);
    }
  }
  if (!keys.empty()) {
    OrderSpaces();
  }
  return created;
}

void CompassSpacesSyncBridge::OrderSpaces() {
  std::vector<std::pair<syncer::UniquePosition, SpaceModel*>> positioned;
  std::vector<SpaceModel*> others;
  for (const auto& space : list_->spaces()) {
    auto it = mirror_.find(Key(space->id()));
    std::optional<syncer::UniquePosition> position;
    if (it != mirror_.end() && it->second.has_space() &&
        it->second.space().has_directory_position()) {
      position = ValidPosition(it->second.space().directory_position());
    }
    if (position) {
      positioned.emplace_back(*position, space.get());
    } else {
      // Not on the server yet: after the chain's.
      others.push_back(space.get());
    }
  }
  std::ranges::stable_sort(positioned, [](const auto& a, const auto& b) {
    return a.first.LessThan(b.first);
  });
  std::vector<SpaceModel*> order;
  for (const auto& [position, space] : positioned) {
    order.push_back(space);
  }
  order.insert(order.end(), others.begin(), others.end());
  for (size_t i = 0; i < order.size(); ++i) {
    if (list_->spaces()[i].get() != order[i]) {
      // It's further down: the slots before i hold order[0..i).
      list_->MoveSpace(*order[i], i);
    }
  }
}

void CompassSpacesSyncBridge::MergeWithChain(
    const std::vector<std::string>& remote_nodes,
    const std::set<std::string>& remote_spaces) {
  // Favorites: the chain's copy of a site stays, this device's goes.
  std::set<std::string> chain_urls;
  for (const std::string& key : remote_nodes) {
    const sync_pb::CompassNode& node = mirror_[key].node();
    const base::Uuid id = ParseKey(key);
    if (id.is_valid() && node.section() == kSectionFavorites &&
        node.type() == kNodeTab && !FindNode(id)) {
      chain_urls.insert(node.url());
    }
  }
  SpaceModel& favorites = list_->favorites();
  std::vector<base::Uuid> duplicates;
  for (const auto& child : favorites.pinned_root()->children()) {
    if (child->type() == Type::kTab && !mirror_.contains(Key(child->id())) &&
        chain_urls.contains(child->url().possibly_invalid_spec())) {
      duplicates.push_back(child->id());
    }
  }
  for (const base::Uuid& id : duplicates) {
    if (SidebarNode* node = favorites.FindById(id)) {
      favorites.Remove(node);
    }
  }

  // A joining device's untouched starting space ("Home", nothing pinned)
  // gives way to the chain's spaces: its Today goes to the chain's first.
  SpaceModel* first = nullptr;
  for (const auto& space : list_->spaces()) {
    if (remote_spaces.contains(Key(space->id()))) {
      first = space.get();
      break;
    }
  }
  if (!first) {
    return;
  }
  std::vector<SpaceModel*> unused;
  for (const auto& space : list_->spaces()) {
    if (!mirror_.contains(Key(space->id())) && space->name() == u"Home" &&
        space->pinned_root()->children().empty()) {
      unused.push_back(space.get());
    }
  }
  for (SpaceModel* space : unused) {
    std::vector<base::Uuid> today;
    for (const auto& child : space->today_root()->children()) {
      today.push_back(child->id());
    }
    for (const base::Uuid& id : today) {
      SidebarNode* node = space->FindById(id);
      if (node && node->parent() == space->today_root()) {
        MoveNode(node, *first, first->today_root(),
                 first->today_root()->children().size());
      }
    }
    DeleteSpace(*space);
  }
}

void CompassSpacesSyncBridge::ApplyNodes(const std::vector<std::string>& keys,
                                         const std::set<std::string>& deleted) {
  // 1. New nodes (at their section's root for now) and changed content.
  std::vector<std::string> to_place;
  std::set<std::string> archive_roots;
  for (const std::string& key : keys) {
    auto it = mirror_.find(key);
    if (it == mirror_.end() || !it->second.has_node()) {
      continue;
    }
    const sync_pb::CompassNode remote = it->second.node();
    const base::Uuid id = ParseKey(key);
    if (!id.is_valid()) {
      continue;
    }

    if (remote.section() == kSectionArchive) {
      // Archived on another device; its entry is rebuilt below.
      archive_roots.insert(remote.parent_uuid().empty() ? key
                                                        : remote.parent_uuid());
      continue;
    }
    // Restored, or never archived.
    DropFromArchives(key);
    SidebarNode* node = FindNode(id);

    SpaceModel* target = TargetModel(remote);
    const std::optional<Type> type = TypeFromProto(remote.type());
    if (!target || !type ||
        (target == &list_->favorites() && *type != Type::kTab)) {
      // Not here (yet), or not something this version can show.
      continue;
    }
    if (!node) {
      if (list_->FindSpace(id)) {
        continue;
      }
      SidebarNode* root = RootFor(*target, remote.section());
      const size_t end = root->children().size();
      const std::u16string title = base::UTF8ToUTF16(remote.title());
      switch (*type) {
        case Type::kTab:
          node = target->AddTab(root, end, GURL(remote.url()), title, id);
          break;
        case Type::kFolder:
          node = target->AddFolder(root, end, title, id);
          break;
        case Type::kSplit:
          node = target->AddSplit(root, end, id);
          break;
        case Type::kRoot:
          break;
      }
      if (!node) {
        continue;
      }
    } else if (node->type() != *type) {
      // A node never changes type.
      continue;
    }

    // Content. A live tab whose URL changed keeps its page (it isn't
    // navigated); its next navigation here commits its own URL (see
    // TabStripBridge::UpdateNode()). A page used here whose change waits to
    // be sent keeps its own URL and title: it commits them next, last.
    SpaceModel* model = ModelOf(node);
    const std::u16string title = base::UTF8ToUTF16(remote.title());
    if (node->type() == Type::kTab) {
      if (!throttled_.contains(key)) {
        const GURL url(remote.url());
        if (node->url() != url) {
          model->SetUrl(node, url);
        }
        if (node->title() != title) {
          model->SetTitle(node, title);
        }
      }
      model->SetCustomTitle(node, base::UTF8ToUTF16(remote.custom_title()));
      // Every device commits its own use: the latest wins.
      if (remote.has_last_active_time()) {
        const base::Time last_active = FromProtoTime(remote.last_active_time());
        if (last_active > node->last_active()) {
          model->SetLastActive(node, last_active);
        }
      }
    } else if (node->type() == Type::kFolder) {
      if (node->title() != title) {
        model->SetTitle(node, title);
      }
      model->SetCollapsed(node, remote.collapsed());
    }
    to_place.push_back(key);
  }

  // 2. Deletions.
  ApplyNodeDeletions(deleted, archive_roots);

  // 3. Placement, parents first. Parked nodes may find their parent now.
  std::set<std::string> listed(to_place.begin(), to_place.end());
  std::vector<const SidebarNode*> roots = {list_->favorites().pinned_root()};
  for (const auto& space : list_->spaces()) {
    roots.push_back(space->pinned_root());
    roots.push_back(space->today_root());
  }
  for (const SidebarNode* root : roots) {
    for (const auto& child : root->children()) {
      const std::string key = Key(child->id());
      auto it = mirror_.find(key);
      if (it != mirror_.end() && it->second.has_node() &&
          !it->second.node().parent_uuid().empty() && !listed.contains(key)) {
        to_place.push_back(key);
        listed.insert(key);
      }
    }
  }
  std::map<std::string, int> depth;
  for (const std::string& key : to_place) {
    int d = 0;
    std::set<std::string> seen = {key};
    for (auto it = mirror_.find(key);
         it != mirror_.end() && it->second.has_node() &&
         !it->second.node().parent_uuid().empty() &&
         seen.insert(it->second.node().parent_uuid()).second;
         it = mirror_.find(it->second.node().parent_uuid())) {
      ++d;
    }
    depth[key] = d;
  }
  std::ranges::stable_sort(to_place, [&depth](const std::string& a,
                                              const std::string& b) {
    return depth[a] < depth[b];
  });
  for (const std::string& key : to_place) {
    PlaceNode(key, deleted);
  }

  // 4. Splits the batch left with fewer than two panes.
  NormalizeSplits();

  // 5. Entries archived elsewhere.
  ApplyArchives(archive_roots);
}

void CompassSpacesSyncBridge::ApplyNodeDeletions(
    const std::set<std::string>& deleted,
    std::set<std::string>& archive_roots) {
  // Archived entries. One that has left the server for its age stays here.
  const base::Time expired = base::Time::Now() - kArchiveSyncPeriod;
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    if (!id.is_valid()) {
      continue;
    }
    for (const auto& space : list_->spaces()) {
      if (const ArchivedEntry* entry = space->FindArchived(id)) {
        if (entry->archived_at >= expired) {
          space->RemoveArchived(id);
        }
        continue;
      }
      for (const ArchivedEntry& entry : space->archive()) {
        if (std::ranges::contains(entry.tabs, id, &ArchivedEntry::Tab::id)) {
          // A pane: its entry is rebuilt from what's left.
          archive_roots.insert(Key(entry.id));
        }
      }
    }
  }

  // A Today entry the focused window shows stays: it's re-created, so the
  // other devices get it back (docs/design/sync.md, "Today across devices").
  std::set<std::string> kept;
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    SidebarNode* node = id.is_valid() ? FindNode(id) : nullptr;
    SpaceModel* space = node ? list_->FindSpaceOf(node) : nullptr;
    if (!space || !space->today_root()->Contains(node)) {
      continue;
    }
    const SidebarNode* entry = node;
    if (entry->type() == Type::kTab &&
        entry->parent()->type() == Type::kSplit) {
      entry = entry->parent();
    }
    if (entry->type() != Type::kFolder && delegate_ &&
        delegate_->IsShownInFocusedWindow(*entry)) {
      kept.insert(Key(entry->id()));
    }
  }
  const auto removing = [&](const SidebarNode* node) {
    if (!deleted.contains(Key(node->id()))) {
      return false;
    }
    for (const SidebarNode* n = node; n; n = n->parent()) {
      if (kept.contains(Key(n->id()))) {
        return false;
      }
    }
    return true;
  };

  // Entries added here to a folder deleted elsewhere aren't lost: they go to
  // the section's root (and the next pass commits that).
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    SidebarNode* node = id.is_valid() ? FindNode(id) : nullptr;
    if (!node || !IsSynced(node) || !removing(node)) {
      continue;
    }
    SpaceModel* model = ModelOf(node);
    SidebarNode* root = model->today_root()->Contains(node)
                            ? model->today_root()
                            : model->pinned_root();
    std::vector<base::Uuid> rescued;
    for (const auto& child : node->children()) {
      if (!removing(child.get())) {
        rescued.push_back(child->id());
      }
    }
    for (const base::Uuid& child_id : rescued) {
      SidebarNode* child = model->FindById(child_id);
      if (child && model->CanMove(child, root)) {
        model->Move(child, root, root->children().size());
      }
    }
  }
  // Moving panes out can remove their split, so go by ID. Removing an entry
  // closes its live tabs, as a local delete does.
  for (const std::string& key : deleted) {
    const base::Uuid id = ParseKey(key);
    SidebarNode* node = id.is_valid() ? FindNode(id) : nullptr;
    if (node && IsSynced(node) && removing(node)) {
      ModelOf(node)->Remove(node);
    }
  }

  // The server won't take a deleted entity back under the same ID, so the
  // kept entries get new ones.
  for (const std::string& key : kept) {
    if (SidebarNode* node = FindNode(ParseKey(key))) {
      SidebarNode* copy = Recreate(node);
      VLOG(1) << "[compass-sync] spaces: kept " << key
              << ", closed elsewhere but shown in the focused window, as "
              << Key(copy->id());
    }
  }
}

void CompassSpacesSyncBridge::ApplyArchives(
    const std::set<std::string>& roots) {
  for (const std::string& root_key : roots) {
    auto it = mirror_.find(root_key);
    const base::Uuid id = ParseKey(root_key);
    if (it == mirror_.end() || !it->second.has_node() || !id.is_valid() ||
        it->second.node().section() != kSectionArchive) {
      continue;
    }
    const sync_pb::CompassNode root = it->second.node();
    const base::Uuid space_id = ParseKey(root.space_uuid());
    SpaceModel* space =
        space_id.is_valid() ? list_->FindSpace(space_id) : nullptr;
    if (!space) {
      // Waits for its space.
      continue;
    }

    ArchivedEntry entry;
    entry.id = id;
    entry.archived_at = root.has_archived_time()
                            ? FromProtoTime(root.archived_time())
                            : base::Time::Now();
    if (root.type() == kNodeTab) {
      entry.tabs.push_back({base::UTF8ToUTF16(root.title()), GURL(root.url()),
                            id});
    } else if (root.type() == kNodeSplit) {
      std::vector<std::pair<std::optional<syncer::UniquePosition>,
                            const sync_pb::CompassNode*>>
          panes;
      for (const auto& [key, specifics] : mirror_) {
        if (specifics.has_node() &&
            specifics.node().section() == kSectionArchive &&
            specifics.node().parent_uuid() == root_key &&
            specifics.node().type() == kNodeTab &&
            ParseKey(key).is_valid()) {
          panes.emplace_back(ValidPosition(specifics.node().position()),
                             &specifics.node());
        }
      }
      // By position; panes without one last.
      std::ranges::stable_sort(panes, [](const auto& a, const auto& b) {
        if (!a.first || !b.first) {
          return a.first.has_value() && !b.first.has_value();
        }
        return a.first->LessThan(*b.first);
      });
      for (const auto& [position, pane] : panes) {
        if (entry.tabs.size() == kMaxSplitPanes) {
          break;
        }
        entry.tabs.push_back({base::UTF8ToUTF16(pane->title()),
                              GURL(pane->url()), ParseKey(pane->uuid())});
      }
    } else {
      continue;
    }

    // The entry and its panes leave Today (or wherever they are here),
    // closing their tabs, unless the entry is in use here: then it stays,
    // seen now, and the next pass puts it back in Today everywhere.
    std::vector<base::Uuid> retiring = {id};
    for (const ArchivedEntry::Tab& tab : entry.tabs) {
      retiring.push_back(tab.id);
    }
    bool in_use = false;
    for (const base::Uuid& retiring_id : retiring) {
      const SidebarNode* node = FindNode(retiring_id);
      if (!node) {
        continue;
      }
      if (node->type() == Type::kTab && node->parent()->type() == Type::kSplit) {
        node = node->parent();
      }
      if (delegate_ && delegate_->IsInUse(*node)) {
        in_use = true;
      }
    }
    if (in_use) {
      const base::Time now = base::Time::Now();
      for (const base::Uuid& retiring_id : retiring) {
        SidebarNode* node = FindNode(retiring_id);
        if (node && node->type() == Type::kTab) {
          ModelOf(node)->SetLastActive(node, now);
        }
      }
      VLOG(1) << "[compass-sync] spaces: kept " << root_key
              << ", archived elsewhere but in use here";
      continue;
    }
    for (const base::Uuid& retiring_id : retiring) {
      if (SidebarNode* node = FindNode(retiring_id)) {
        ModelOf(node)->Remove(node);
      }
    }
    for (const auto& other : list_->spaces()) {
      if (other.get() != space && other->FindArchived(id)) {
        other->RemoveArchived(id);
      }
    }
    // No panes yet: the entry waits for them.
    space->PutArchived(std::move(entry));
  }
}

void CompassSpacesSyncBridge::DropFromArchives(const std::string& key) {
  const base::Uuid id = ParseKey(key);
  if (!id.is_valid()) {
    return;
  }
  for (const auto& space : list_->spaces()) {
    if (space->FindArchived(id)) {
      space->RemoveArchived(id);
    }
  }
}

void CompassSpacesSyncBridge::DedupeToday(
    const std::vector<std::string>& remote_nodes) {
  const std::set<std::string> remote(remote_nodes.begin(), remote_nodes.end());
  for (const auto& space : list_->spaces()) {
    // Tabs outside splits, depth first: the chain's by URL, and this
    // device's own.
    std::map<GURL, SidebarNode*> chain;
    std::vector<base::Uuid> own;
    std::vector<SidebarNode*> stack = {space->today_root()};
    while (!stack.empty()) {
      SidebarNode* parent = stack.back();
      stack.pop_back();
      for (auto child = parent->children().rbegin();
           child != parent->children().rend(); ++child) {
        SidebarNode* node = child->get();
        if (node->type() == Type::kFolder) {
          stack.push_back(node);
          continue;
        }
        if (node->type() != Type::kTab) {
          continue;
        }
        const std::string key = Key(node->id());
        if (remote.contains(key)) {
          chain.emplace(node->url(), node);
        } else if (!mirror_.contains(key)) {
          own.push_back(node->id());
        }
      }
    }
    for (const base::Uuid& id : own) {
      SidebarNode* node = space->FindById(id);
      auto it = node ? chain.find(node->url()) : chain.end();
      if (it == chain.end()) {
        continue;
      }
      SidebarNode* keep = it->second;
      if (node->last_active() > keep->last_active()) {
        space->SetLastActive(keep, node->last_active());
      }
      if (delegate_) {
        delegate_->TransferLiveTab(*node, *keep);
      }
      space->Remove(node);
    }
  }
}

SidebarNode* CompassSpacesSyncBridge::Recreate(SidebarNode* node) {
  SpaceModel* model = ModelOf(node);
  SidebarNode* parent = node->parent();
  SidebarNode* copy =
      CopyNode(*model, *node, parent, parent->GetIndexOf(node));
  // Its live tabs went to the copy, so this closes none.
  model->Remove(node);
  return copy;
}

SidebarNode* CompassSpacesSyncBridge::CopyNode(SpaceModel& model,
                                               const SidebarNode& node,
                                               SidebarNode* parent,
                                               size_t index) {
  SidebarNode* copy = nullptr;
  switch (node.type()) {
    case Type::kTab:
      copy = model.AddTab(parent, index, node.url(), node.title());
      model.SetCustomTitle(copy, node.custom_title());
      model.SetMuted(copy, node.muted());
      if (!node.last_active().is_null()) {
        model.SetLastActive(copy, node.last_active());
      }
      if (delegate_) {
        delegate_->TransferLiveTab(node, *copy);
      }
      break;
    case Type::kSplit:
      copy = model.AddSplit(parent, index);
      for (const auto& pane : node.children()) {
        CopyNode(model, *pane, copy, copy->children().size());
      }
      break;
    case Type::kFolder:
      copy = model.AddFolder(parent, index, node.title());
      model.SetCollapsed(copy, node.collapsed());
      for (const auto& child : node.children()) {
        CopyNode(model, *child, copy, copy->children().size());
      }
      break;
    case Type::kRoot:
      NOTREACHED();
  }
  return copy;
}

void CompassSpacesSyncBridge::PlaceNode(const std::string& key,
                                        const std::set<std::string>& deleted) {
  auto it = mirror_.find(key);
  if (it == mirror_.end() || !it->second.has_node()) {
    return;
  }
  const sync_pb::CompassNode remote = it->second.node();
  SidebarNode* node = FindNode(ParseKey(key));
  SpaceModel* target = TargetModel(remote);
  if (!node || !target) {
    return;
  }
  SidebarNode* root = RootFor(*target, remote.section());
  SidebarNode* parent = root;
  // Whether it goes (or stays) at the end of the root rather than where its
  // position says: that position is among its parent's children, not the
  // root's.
  bool at_root_end = false;
  if (!remote.parent_uuid().empty()) {
    const base::Uuid parent_id = ParseKey(remote.parent_uuid());
    SidebarNode* wanted = parent_id.is_valid() ? FindNode(parent_id) : nullptr;
    if (deleted.contains(remote.parent_uuid())) {
      // Its parent is gone: it stays at the end of the root, where
      // ApplyNodeDeletions() put it, and the next pass commits that.
      unparked_.insert(key);
      at_root_end = true;
    } else if (wanted) {
      SpaceModel* source = ModelOf(node);
      // A cycle (the parent is inside it) or an invalid placement (a folder
      // in a split) leaves it at the root; the next pass commits that.
      const bool valid =
          root->Contains(wanted) && !node->Contains(wanted) &&
          (source == target ? target->CanMove(node, wanted)
                            : source->CanMoveToSpace(node, *target, wanted));
      if (valid) {
        parent = wanted;
      }
    } else {
      // Its parent hasn't come yet: it waits at the end of the root,
      // parked.
      at_root_end = true;
    }
  }
  if (at_root_end && node->parent() == root) {
    return;
  }
  const size_t index = at_root_end
                           ? root->children().size()
                           : InsertionIndex(*parent, node, remote.position());
  MoveNode(node, *target, parent, index);
}

void CompassSpacesSyncBridge::NormalizeSplits() {
  std::vector<base::Uuid> splits;
  for (const auto& space : list_->spaces()) {
    CollectSplits(*space->pinned_root(), splits);
    CollectSplits(*space->today_root(), splits);
  }
  for (const base::Uuid& id : splits) {
    SidebarNode* split = FindNode(id);
    if (!split || split->type() != Type::kSplit) {
      continue;
    }
    SpaceModel* model = ModelOf(split);
    if (split->children().empty()) {
      model->Remove(split);
    } else if (split->children().size() == 1) {
      model->Unsplit(split);
    }
  }
}

// Model helpers ---------------------------------------------------------------

SidebarNode* CompassSpacesSyncBridge::FindNode(const base::Uuid& id) const {
  if (SidebarNode* node = list_->favorites().FindById(id)) {
    return node;
  }
  for (const auto& space : list_->spaces()) {
    if (SidebarNode* node = space->FindById(id)) {
      return node;
    }
  }
  return nullptr;
}

SpaceModel* CompassSpacesSyncBridge::ModelOf(const SidebarNode* node) const {
  if (list_->favorites().Contains(node)) {
    return &list_->favorites();
  }
  return list_->FindSpaceOf(node);
}

bool CompassSpacesSyncBridge::IsSynced(const SidebarNode* node) const {
  return list_->favorites().pinned_root()->Contains(node) ||
         list_->FindSpaceOf(node);
}

SpaceModel* CompassSpacesSyncBridge::TargetModel(
    const sync_pb::CompassNode& node) const {
  switch (node.section()) {
    case kSectionFavorites:
      return &list_->favorites();
    case kSectionPinned:
    case kSectionToday: {
      const base::Uuid id = ParseKey(node.space_uuid());
      return id.is_valid() ? list_->FindSpace(id) : nullptr;
    }
    default:
      // The archive isn't a tree (see ApplyArchives()).
      return nullptr;
  }
}

void CompassSpacesSyncBridge::MoveNode(SidebarNode* node,
                                       SpaceModel& target,
                                       SidebarNode* parent,
                                       size_t index) {
  SpaceModel* source = ModelOf(node);
  if (!source) {
    return;
  }
  if (source == &target) {
    if (node->parent() == parent && parent->GetIndexOf(node) == index) {
      return;
    }
    if (!target.CanMove(node, parent)) {
      return;
    }
    // |index| leaves |node| out.
    const size_t others =
        parent->children().size() - (node->parent() == parent ? 1 : 0);
    target.Move(node, parent, std::min(index, others));
    return;
  }
  if (!source->CanMoveToSpace(node, target, parent)) {
    return;
  }
  index = std::min(index, parent->children().size());
  if (delegate_) {
    delegate_->MoveEntry(node, target, parent, index);
  } else {
    source->MoveToSpace(node, target, parent, index);
  }
}

void CompassSpacesSyncBridge::DeleteSpace(SpaceModel& space) {
  if (delegate_) {
    delegate_->DeleteSpace(space);
  } else if (list_->spaces().size() > 1) {
    list_->RemoveSpace(space);
  }
}

size_t CompassSpacesSyncBridge::InsertionIndex(
    const SidebarNode& parent,
    const SidebarNode* node,
    const sync_pb::UniquePosition& position) const {
  const std::optional<syncer::UniquePosition> target = ValidPosition(position);
  size_t index = 0;
  for (const auto& child : parent.children()) {
    if (child.get() == node) {
      continue;
    }
    if (target) {
      // Before the first sibling the server has after it, or the first one
      // it doesn't have there (this device's own, not sent yet, or parked):
      // after an initial merge, the chain's entries come first.
      std::optional<syncer::UniquePosition> sibling =
          MirrorPosition(*child, parent);
      if (!sibling || target->LessThan(*sibling)) {
        return index;
      }
    }
    ++index;
  }
  return index;
}

std::optional<syncer::UniquePosition> CompassSpacesSyncBridge::MirrorPosition(
    const SidebarNode& child,
    const SidebarNode& parent) const {
  auto it = mirror_.find(Key(child.id()));
  if (it == mirror_.end() || !it->second.has_node() ||
      it->second.node().parent_uuid() != ParentKey(parent) ||
      !it->second.node().has_position()) {
    return std::nullopt;
  }
  if (parent.type() == Type::kRoot) {
    // Every root is "": the section tells them apart.
    SpaceModel* model = ModelOf(&parent);
    const int section = model == &list_->favorites() ? kSectionFavorites
                        : model && &parent == model->today_root()
                            ? kSectionToday
                            : kSectionPinned;
    if (it->second.node().section() != section) {
      return std::nullopt;
    }
  }
  return ValidPosition(it->second.node().position());
}

std::string CompassSpacesSyncBridge::ParentKey(
    const SidebarNode& parent) const {
  return parent.type() == Type::kRoot ? std::string() : Key(parent.id());
}

void CompassSpacesSyncBridge::Observe(SpaceModel& model) {
  // Removes first, so a model already observed isn't observed twice.
  model.RemoveObserver(this);
  model.AddObserver(this);
}

// The rest of DataTypeSyncBridge ----------------------------------------------

std::unique_ptr<syncer::DataBatch> CompassSpacesSyncBridge::GetDataForCommit(
    StorageKeyList storage_keys) {
  auto batch = std::make_unique<syncer::MutableDataBatch>();
  for (const std::string& key : storage_keys) {
    auto it = mirror_.find(key);
    if (it == mirror_.end()) {
      continue;
    }
    auto data = std::make_unique<syncer::EntityData>();
    data->name = key;
    *data->specifics.mutable_compass_spaces() = it->second;
    batch->Put(key, std::move(data));
  }
  return batch;
}

std::unique_ptr<syncer::DataBatch>
CompassSpacesSyncBridge::GetAllDataForDebugging() {
  StorageKeyList keys;
  for (const auto& [key, specifics] : mirror_) {
    keys.push_back(key);
  }
  return GetDataForCommit(std::move(keys));
}

std::string CompassSpacesSyncBridge::GetClientTag(
    const syncer::EntityData& entity_data) const {
  return GetStorageKey(entity_data);
}

std::string CompassSpacesSyncBridge::GetStorageKey(
    const syncer::EntityData& entity_data) const {
  const sync_pb::CompassSpacesSpecifics& specifics =
      entity_data.specifics.compass_spaces();
  if (specifics.has_space()) {
    return specifics.space().uuid();
  }
  if (specifics.has_profile()) {
    return specifics.profile().uuid();
  }
  if (specifics.has_favicon()) {
    return specifics.favicon().uuid();
  }
  return specifics.node().uuid();
}

syncer::ConflictResolution CompassSpacesSyncBridge::ResolveConflict(
    const std::string& storage_key,
    const syncer::EntityData& remote_data) const {
  const syncer::ConflictResolution resolution =
      syncer::DataTypeSyncBridge::ResolveConflict(storage_key, remote_data);
  // A delete from another device (a tab closed there) that meets a change
  // here not yet committed (a title, a last-active time) never reaches
  // ApplyIncrementalSyncChanges(): the processor keeps the entity and
  // commits it again, which undeletes it on the server. That's the other
  // way an entry is kept; say so as the focused-window rule does.
  if (remote_data.is_deleted() &&
      resolution == syncer::ConflictResolution::kUseLocal) {
    VLOG(1) << "[compass-sync] spaces: kept " << storage_key
            << ", deleted elsewhere while changed here; it goes up again";
  }
  return resolution;
}

void CompassSpacesSyncBridge::ApplyDisableSyncChanges(
    std::unique_ptr<syncer::MetadataChangeList> delete_metadata_change_list) {
  // Leaving the chain keeps the spaces; only what sync knew goes.
  mirror_.clear();
  unparked_.clear();
  last_commit_.clear();
  throttled_.clear();
  throttle_timer_.Stop();
  archive_expiry_timer_.Stop();
  if (store_) {
    store_->DeleteAllDataAndMetadata(std::move(delete_metadata_change_list),
                                     base::DoNothing());
  }
}

sync_pb::EntitySpecifics
CompassSpacesSyncBridge::TrimAllSupportedFieldsFromRemoteSpecifics(
    const sync_pb::EntitySpecifics& entity_specifics) const {
  // The mirror keeps every field, known or not, so the processor needn't.
  return sync_pb::EntitySpecifics();
}

bool CompassSpacesSyncBridge::IsEntityDataValid(
    const syncer::EntityData& entity_data) const {
  if (!entity_data.specifics.has_compass_spaces()) {
    return false;
  }
  const sync_pb::CompassSpacesSpecifics& specifics =
      entity_data.specifics.compass_spaces();
  if (!specifics.has_space() && !specifics.has_node() &&
      !specifics.has_profile() && !specifics.has_favicon()) {
    return false;
  }
  if (specifics.has_favicon()) {
    // Keyed by its host, so two devices' icons for a host are one entity.
    const sync_pb::CompassFavicon& favicon = specifics.favicon();
    return !favicon.host().empty() &&
           favicon.uuid() == FaviconKey(favicon.host());
  }
  return ParseKey(GetStorageKey(entity_data)).is_valid();
}

// Observers -------------------------------------------------------------------

void CompassSpacesSyncBridge::OnSpaceAdded(SpaceModel& space) {
  Observe(space);
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnSpaceUpdated(const SpaceModel& space) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnSpaceWillBeRemoved(SpaceModel& space) {
  space.RemoveObserver(this);
}

void CompassSpacesSyncBridge::OnSpaceRemoved() {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnSpaceMoved(const SpaceModel& space) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeAdded(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeMoved(const SidebarNode& node,
                                          const SidebarNode& old_parent,
                                          size_t old_index) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeChanged(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeWillBeRemoved(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeWillMoveToSpace(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnNodeLastActiveChanged(const SidebarNode& node) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

void CompassSpacesSyncBridge::OnArchiveChanged(const SpaceModel& space) {
  if (!applying_remote_) {
    ScheduleSendLocalChanges();
  }
}

#undef COMPASS_SET_STRING

}  // namespace compass::sync
