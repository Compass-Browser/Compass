// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_ARC_SIDEBAR_H_
#define COMPASS_BROWSER_ARC_IMPORT_ARC_SIDEBAR_H_

#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "base/values.h"
#include "compass/browser/sidebar/space_model.h"
#include "url/gurl.h"

// Reading Arc's sidebar and profiles (docs/design/arc-import.md): what
// StorableSidebar.json and Arc's Local State hold, as plain data for the
// importer. No browser dependencies.
namespace compass::arc {

// An entry of Arc's sidebar.
struct Item {
  enum class Kind { kTab, kFolder, kSplit };

  Item();
  Item(const Item&);
  Item(Item&&);
  Item& operator=(const Item&);
  Item& operator=(Item&&);
  ~Item();

  Kind kind = Kind::kTab;
  // Arc's item ID.
  std::string id;
  // A folder's name, or a tab's own title if the user gave it one.
  std::u16string title;
  // Tabs: the page's title and URL when Arc saved them.
  std::u16string saved_title;
  GURL url;
  base::Time last_active;
  bool muted = false;
  // Folders and splits.
  std::vector<Item> children;
};

// An entry of Arc's archive (StorableArchiveItems.json): a tab archived from
// a space.
struct ArchivedItem {
  // Arc's item ID.
  std::string id;
  // The space it was archived from.
  std::string space_id;
  // The user's own title for the tab, else the page's (may be empty).
  std::u16string title;
  GURL url;
  base::Time archived_at;
};

struct Space {
  Space();
  Space(const Space&);
  Space(Space&&);
  Space& operator=(const Space&);
  Space& operator=(Space&&);
  ~Space();

  std::string id;
  std::u16string name;
  // The Arc profile's folder ("Default", "Profile 1", ...).
  std::string profile_dir;
  SpaceColor color = SpaceColor::kGrey;
  // After |color|, for a gradient theme (SpaceModel::gradient()).
  std::vector<SpaceColor> gradient;
  // The theme's modifiers (SpaceModel::intensity() and grain()).
  double intensity = kDefaultSpaceIntensity;
  double grain = 0;
  SpaceIcon icon;
  std::vector<Item> pinned;
  std::vector<Item> today;
  // Its archive, newest first, at most kMaxArchivedEntries (AddArchive()).
  std::vector<ArchivedItem> archive;
};

struct Profile {
  Profile();
  Profile(const Profile&);
  Profile(Profile&&);
  Profile& operator=(const Profile&);
  Profile& operator=(Profile&&);
  ~Profile();

  // Its folder in Arc's User Data.
  std::string dir;
  // As Arc shows it ("Your Arc" for the default one).
  std::u16string name;
  // Arc's "top apps".
  std::vector<Item> favorites;
};

struct Sidebar {
  Sidebar();
  Sidebar(const Sidebar&);
  Sidebar(Sidebar&&);
  Sidebar& operator=(const Sidebar&);
  Sidebar& operator=(Sidebar&&);
  ~Sidebar();

  // Those with a space, in the order of their first space, then the others
  // Local State lists.
  std::vector<Profile> profiles;
  // In Arc's order.
  std::vector<Space> spaces;
};

// Reads |storable_sidebar| (StorableSidebar.json) and |local_state| (Arc's
// User Data/Local State, for profile names; may be null). Returns nullopt if
// it isn't a sidebar Compass understands.
std::optional<Sidebar> ParseSidebar(const base::DictValue& storable_sidebar,
                                    const base::DictValue* local_state);

// Reads |storable_archive| (StorableArchiveItems.json): the tabs Arc archived
// from spaces, in the file's order. Entries archived from Little Arc
// windows (no space) and other kinds of entries are left out.
std::vector<ArchivedItem> ParseArchive(const base::DictValue& storable_archive);

// Gives each of |spaces| its entries of |archive|: the newest
// kMaxArchivedEntries, newest first, as Compass keeps no more (older ones
// are dropped).
void AddArchive(std::vector<ArchivedItem> archive, std::vector<Space>& spaces);

// The space color for an Arc theme color (components in 0-1; Arc's extended
// sRGB may go beyond, they're clamped): the same color, or grey for an
// unsaturated one.
SpaceColor ArcSpaceColor(double red, double green, double blue);

// The Compass symbol (resources/sidebar/space_icons.ts) for one of Arc's
// space symbols, or nullopt if there's none like it.
std::optional<std::string_view> ArcSymbolToCompass(std::string_view arc_name);

// Arc's times are seconds since 2001-01-01 (Apple's reference date).
base::Time FromArcTime(double seconds);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_ARC_SIDEBAR_H_
