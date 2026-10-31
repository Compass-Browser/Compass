// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/arc_sidebar.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "base/containers/fixed_flat_map.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"

namespace compass::arc {

Item::Item() = default;
Item::Item(const Item&) = default;
Item::Item(Item&&) = default;
Item& Item::operator=(const Item&) = default;
Item& Item::operator=(Item&&) = default;
Item::~Item() = default;

Space::Space() = default;
Space::Space(const Space&) = default;
Space::Space(Space&&) = default;
Space& Space::operator=(const Space&) = default;
Space& Space::operator=(Space&&) = default;
Space::~Space() = default;

Profile::Profile() = default;
Profile::Profile(const Profile&) = default;
Profile::Profile(Profile&&) = default;
Profile& Profile::operator=(const Profile&) = default;
Profile& Profile::operator=(Profile&&) = default;
Profile::~Profile() = default;

Sidebar::Sidebar() = default;
Sidebar::Sidebar(const Sidebar&) = default;
Sidebar::Sidebar(Sidebar&&) = default;
Sidebar& Sidebar::operator=(const Sidebar&) = default;
Sidebar& Sidebar::operator=(Sidebar&&) = default;
Sidebar::~Sidebar() = default;

namespace {

// Seconds from the Unix epoch to 2001-01-01.
constexpr double kAppleEpochOffset = 978307200;
// Items nest at most this deep (Arc's folders, defensively).
constexpr int kMaxDepth = 32;

constexpr char kDefaultProfileDir[] = "Default";

// Arc's profile references: {"default": true} or
// {"custom": {"_0": {"directoryBasename": "Profile 1"}}}.
std::optional<std::string> ProfileDir(const base::Value& profile) {
  const base::DictValue* dict = profile.GetIfDict();
  if (!dict) {
    return std::nullopt;
  }
  if (dict->FindBool("default").value_or(false)) {
    return kDefaultProfileDir;
  }
  if (const std::string* dir =
          dict->FindStringByDottedPath("custom._0.directoryBasename")) {
    return *dir;
  }
  return std::nullopt;
}

// The objects of a Swift-encoded dictionary (a list alternating keys and
// values), by their "id".
std::map<std::string, const base::DictValue*> ObjectsById(
    const base::ListValue* list) {
  std::map<std::string, const base::DictValue*> objects;
  if (!list) {
    return objects;
  }
  for (const base::Value& entry : *list) {
    const base::DictValue* object = entry.GetIfDict();
    if (!object) {
      continue;
    }
    if (const std::string* id = object->FindString("id")) {
      objects[*id] = object;
    }
  }
  return objects;
}

// The value after |key| in a list of alternating keys and values.
const std::string* ValueAfter(const base::ListValue* list,
                              std::string_view key) {
  if (!list) {
    return nullptr;
  }
  for (size_t i = 0; i + 1 < list->size(); ++i) {
    const std::string* entry = (*list)[i].GetIfString();
    if (entry && *entry == key) {
      return (*list)[i + 1].GetIfString();
    }
  }
  return nullptr;
}

class TreeReader {
 public:
  explicit TreeReader(std::map<std::string, const base::DictValue*> items)
      : items_(std::move(items)) {}

  // The entries in the container |id|, in order.
  std::vector<Item> ReadChildren(const std::string& id, int depth = 0) {
    std::vector<Item> children;
    auto it = items_.find(id);
    if (it == items_.end() || depth > kMaxDepth || !seen_.insert(id).second) {
      return children;
    }
    const base::ListValue* ids = it->second->FindList("childrenIds");
    if (!ids) {
      return children;
    }
    for (const base::Value& child_id : *ids) {
      if (!child_id.is_string()) {
        continue;
      }
      if (std::optional<Item> item = ReadItem(child_id.GetString(), depth)) {
        children.push_back(std::move(*item));
      }
    }
    return children;
  }

 private:
  std::optional<Item> ReadItem(const std::string& id, int depth) {
    auto it = items_.find(id);
    if (it == items_.end()) {
      return std::nullopt;
    }
    const base::DictValue& object = *it->second;
    const base::DictValue* data = object.FindDict("data");
    if (!data) {
      return std::nullopt;
    }
    Item item;
    item.id = id;
    if (const std::string* title = object.FindString("title")) {
      item.title = base::UTF8ToUTF16(*title);
    }
    if (const base::DictValue* tab = data->FindDict("tab")) {
      item.kind = Item::Kind::kTab;
      if (const std::string* url = tab->FindString("savedURL")) {
        item.url = GURL(*url);
      }
      if (!item.url.is_valid()) {
        return std::nullopt;
      }
      if (const std::string* title = tab->FindString("savedTitle")) {
        item.saved_title = base::UTF8ToUTF16(*title);
      }
      if (std::optional<double> time = tab->FindDouble("timeLastActiveAt")) {
        item.last_active = FromArcTime(*time);
      }
      if (const std::string* mute = tab->FindString("savedMuteStatus")) {
        item.muted = *mute == "muted";
      }
      seen_.insert(id);
      return item;
    }
    if (data->Find("splitView")) {
      item.kind = Item::Kind::kSplit;
    } else if (data->Find("list")) {
      item.kind = Item::Kind::kFolder;
    } else {
      // An unknown kind of entry (Arc's notes, easels, ...): skipped.
      return std::nullopt;
    }
    item.children = ReadChildren(id, depth + 1);
    // A split of fewer than two tabs is its tab, if any.
    if (item.kind == Item::Kind::kSplit && item.children.size() < 2) {
      if (item.children.empty()) {
        return std::nullopt;
      }
      return std::move(item.children.front());
    }
    return item;
  }

  std::map<std::string, const base::DictValue*> items_;
  // Guards against cycles.
  std::set<std::string> seen_;
};

// The color of a space: its theme's mid tone.
SpaceColor ReadSpaceColor(const base::DictValue& space) {
  const base::DictValue* color = space.FindDictByDottedPath(
      "customInfo.windowTheme.primaryColorPalette.midTone");
  if (!color) {
    return SpaceColor::kGrey;
  }
  return ArcSpaceColor(color->FindDouble("red").value_or(0),
                           color->FindDouble("green").value_or(0),
                           color->FindDouble("blue").value_or(0));
}

// A space's icon: customInfo.iconType is {"emoji_v2": "🚑", "emoji": 128657}
// or {"icon": "planet"}.
SpaceIcon ReadSpaceIcon(const base::DictValue& space) {
  const base::DictValue* type =
      space.FindDictByDottedPath("customInfo.iconType");
  if (!type) {
    return SpaceIcon();
  }
  std::optional<SpaceIcon> icon;
  if (const std::string* emoji = type->FindString("emoji_v2")) {
    icon = SpaceIcon::Emoji(*emoji);
  } else if (std::optional<int> code_point = type->FindInt("emoji");
             code_point && base::IsValidCharacter(*code_point)) {
    std::string utf8;
    base::WriteUnicodeCharacter(*code_point, &utf8);
    icon = SpaceIcon::Emoji(utf8);
  } else if (const std::string* name = type->FindString("icon")) {
    if (std::optional<std::string_view> symbol = ArcSymbolToCompass(*name)) {
      icon = SpaceIcon::Symbol(*symbol);
    }
  }
  return icon.value_or(SpaceIcon());
}

}  // namespace

std::optional<std::string_view> ArcSymbolToCompass(std::string_view arc_name) {
  // Only "github", "planet" and "colorPallete" (sic) were seen in real Arc
  // data; the rest are Arc's likely names for the same ideas. Brands become
  // what they stand for.
  static constexpr auto kSymbols =
      base::MakeFixedFlatMap<std::string_view, std::string_view>({
          {"airplane", "travel"},     {"bag", "shopping"},
          {"bolt", "bolt"},           {"book", "book"},
          {"briefcase", "work"},      {"bulb", "lightbulb"},
          {"camera", "image"},        {"car", "car"},
          {"cart", "shopping"},       {"chart", "finance"},
          {"code", "code"},           {"colorPallete", "palette"},
          {"compass", "compass"},     {"confetti", "celebration"},
          {"earth", "globe"},         {"flask", "science"},
          {"gamecontroller", "game"}, {"github", "code"},
          {"globe", "globe"},         {"graduationcap", "graduation"},
          {"hammer", "build"},        {"home", "home"},
          {"house", "home"},          {"lightbulb", "lightbulb"},
          {"lock", "lock"},           {"music", "music"},
          {"palette", "palette"},     {"person", "person"},
          {"photo", "image"},         {"planet", "globe"},
          {"shield", "shield"},       {"terminal", "code"},
          {"wallet", "wallet"},       {"work", "work"},
          {"wrench", "build"},
      });
  if (const auto it = kSymbols.find(arc_name); it != kSymbols.end()) {
    return it->second;
  }
  return std::nullopt;
}

base::Time FromArcTime(double seconds) {
  return base::Time::FromSecondsSinceUnixEpoch(seconds + kAppleEpochOffset);
}

SpaceColor ArcSpaceColor(double red, double green, double blue) {
  red = std::clamp(red, 0.0, 1.0);
  green = std::clamp(green, 0.0, 1.0);
  blue = std::clamp(blue, 0.0, 1.0);
  const double max = std::max({red, green, blue});
  const double min = std::min({red, green, blue});
  // Unsaturated or nearly black: Arc's plain themes.
  if (max < 0.08 || (max - min) / max < 0.15) {
    return SpaceColor::kGrey;
  }
  auto to_byte = [](double component) {
    return base::ClampRound<uint8_t>(component * 255);
  };
  return SpaceColor::FromRGB(to_byte(red), to_byte(green), to_byte(blue));
}

std::optional<Sidebar> ParseSidebar(const base::DictValue& storable_sidebar,
                                    const base::DictValue* local_state) {
  const base::ListValue* containers =
      storable_sidebar.FindListByDottedPath("sidebar.containers");
  if (!containers) {
    return std::nullopt;
  }
  const base::DictValue* container = nullptr;
  for (const base::Value& entry : *containers) {
    if (entry.is_dict() && entry.GetDict().FindList("spaces")) {
      container = &entry.GetDict();
      break;
    }
  }
  if (!container) {
    return std::nullopt;
  }

  TreeReader tree(ObjectsById(container->FindList("items")));
  Sidebar sidebar;

  // Profile names, from Local State.
  std::vector<std::pair<std::string, std::u16string>> known_profiles;
  if (local_state) {
    if (const base::DictValue* info_cache =
            local_state->FindDictByDottedPath("profile.info_cache")) {
      for (const auto [dir, info] : *info_cache) {
        const std::string* name =
            info.is_dict() ? info.GetDict().FindString("name") : nullptr;
        known_profiles.emplace_back(
            dir, name ? base::UTF8ToUTF16(*name) : base::UTF8ToUTF16(dir));
      }
    }
  }
  auto profile_for = [&](const std::string& dir) -> Profile& {
    for (Profile& profile : sidebar.profiles) {
      if (profile.dir == dir) {
        return profile;
      }
    }
    Profile profile;
    profile.dir = dir;
    profile.name = base::UTF8ToUTF16(dir);
    for (const auto& [known_dir, name] : known_profiles) {
      if (known_dir == dir) {
        profile.name = name;
      }
    }
    sidebar.profiles.push_back(std::move(profile));
    return sidebar.profiles.back();
  };

  // Spaces, in Arc's order (the spaces list's).
  const base::ListValue* spaces = container->FindList("spaces");
  for (const base::Value& entry : *spaces) {
    const base::DictValue* object = entry.GetIfDict();
    const std::string* id = object ? object->FindString("id") : nullptr;
    if (!id) {
      continue;
    }
    Space space;
    space.id = *id;
    if (const std::string* title = object->FindString("title")) {
      space.name = base::UTF8ToUTF16(*title);
    }
    const base::Value* profile = object->Find("profile");
    space.profile_dir =
        (profile ? ProfileDir(*profile) : std::nullopt)
            .value_or(kDefaultProfileDir);
    space.color = ReadSpaceColor(*object);
    space.icon = ReadSpaceIcon(*object);
    const base::ListValue* container_ids = object->FindList("containerIDs");
    if (const std::string* pinned = ValueAfter(container_ids, "pinned")) {
      space.pinned = tree.ReadChildren(*pinned);
    }
    if (const std::string* today = ValueAfter(container_ids, "unpinned")) {
      space.today = tree.ReadChildren(*today);
    }
    profile_for(space.profile_dir);
    sidebar.spaces.push_back(std::move(space));
  }

  // Favorites: profile, then its container's ID.
  if (const base::ListValue* top_apps =
          container->FindList("topAppsContainerIDs")) {
    for (size_t i = 0; i + 1 < top_apps->size(); i += 2) {
      std::optional<std::string> dir = ProfileDir((*top_apps)[i]);
      const std::string* container_id = (*top_apps)[i + 1].GetIfString();
      if (!dir || !container_id) {
        continue;
      }
      std::vector<Item> favorites = tree.ReadChildren(*container_id);
      // Favorites are tabs only.
      std::erase_if(favorites, [](const Item& item) {
        return item.kind != Item::Kind::kTab;
      });
      profile_for(*dir).favorites = std::move(favorites);
    }
  }

  // Profiles without spaces, as Local State lists them.
  for (const auto& [dir, name] : known_profiles) {
    profile_for(dir);
  }
  return sidebar;
}

}  // namespace compass::arc
