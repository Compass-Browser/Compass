// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_SPACE_MODEL_H_
#define COMPASS_BROWSER_SIDEBAR_SPACE_MODEL_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/function_ref.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "third_party/skia/include/core/SkColor.h"

namespace compass {

// Matches upstream split tabs, which Compass extends from two to four
// (MultiContentsView::kMaxPanes, TabStripModel::AddToNewSplit(); see
// patches/). See ARCHITECTURE.md §9.
inline constexpr size_t kMaxSplitPanes = 4;

// A space's color: grey, which leaves the sidebar untinted, or any opaque
// color, from the palette the space menu offers (the constants below) or
// picked freely. Saved by palette name or as #rrggbb (sidebar_serializer.cc).
// See docs/design/spaces.md, "Space colors".
class SpaceColor {
 public:
  // Grey.
  constexpr SpaceColor() = default;

  static constexpr SpaceColor FromRGB(uint8_t red,
                                      uint8_t green,
                                      uint8_t blue) {
    return SpaceColor(SkColorSetRGB(red, green, blue));
  }
  // |color|'s alpha is ignored.
  static constexpr SpaceColor FromSkColor(SkColor color) {
    return SpaceColor(SkColorSetA(color, SK_AlphaOPAQUE));
  }

  static const SpaceColor kGrey;
  static const SpaceColor kBlue;
  static const SpaceColor kPurple;
  static const SpaceColor kPink;
  static const SpaceColor kRed;
  static const SpaceColor kOrange;
  static const SpaceColor kYellow;
  static const SpaceColor kGreen;

  constexpr bool is_grey() const { return !tint_.has_value(); }
  // What the sidebar is tinted with, opaque; nullopt for grey.
  constexpr std::optional<SkColor> tint() const { return tint_; }

  friend constexpr bool operator==(const SpaceColor&,
                                   const SpaceColor&) = default;

 private:
  constexpr explicit SpaceColor(SkColor tint) : tint_(tint) {}

  std::optional<SkColor> tint_;
};

inline constexpr SpaceColor SpaceColor::kGrey{};
inline constexpr SpaceColor SpaceColor::kBlue = SpaceColor::FromRGB(59, 130, 246);
inline constexpr SpaceColor SpaceColor::kPurple =
    SpaceColor::FromRGB(139, 92, 246);
inline constexpr SpaceColor SpaceColor::kPink = SpaceColor::FromRGB(236, 72, 153);
inline constexpr SpaceColor SpaceColor::kRed = SpaceColor::FromRGB(239, 68, 68);
inline constexpr SpaceColor SpaceColor::kOrange =
    SpaceColor::FromRGB(249, 115, 22);
inline constexpr SpaceColor SpaceColor::kYellow =
    SpaceColor::FromRGB(234, 179, 8);
inline constexpr SpaceColor SpaceColor::kGreen = SpaceColor::FromRGB(34, 197, 94);

// A space's color and its gradient colors, at most this many in all.
inline constexpr size_t kMaxSpaceGradientColors = 3;

// How strongly a space's color tints the sidebar and frame, 0 to 1: the
// tint's opacity is kMaxSpaceTintOpacity times this, capped at 100%, as on
// the phone (SpaceTint in ios/App/Views/Theme.swift). The default gives
// about 34%; 1 paints the color itself, as Arc's full intensity does.
inline constexpr double kMaxSpaceTintOpacity = 1.1;
inline constexpr double kDefaultSpaceIntensity = 0.31;

// The tint's opacity, 0 to 1, for an intensity.
inline double SpaceTintOpacity(double intensity) {
  return std::clamp(kMaxSpaceTintOpacity * intensity, 0.0, 1.0);
}

// Whether a space's windows follow the system's light or dark mode or keep
// one (docs/design/spaces.md, "Theme editor"). Saved by name.
enum class SpaceAppearance { kAuto, kLight, kDark };

// The space menu's palette, by the names files save them under. The sidebar
// page (resources/sidebar/app.ts) shows the same colors.
struct NamedSpaceColor {
  std::string_view name;
  SpaceColor color;
};
inline constexpr auto kSpaceColorPalette = std::to_array<NamedSpaceColor>({
    {"grey", SpaceColor::kGrey},
    {"blue", SpaceColor::kBlue},
    {"purple", SpaceColor::kPurple},
    {"pink", SpaceColor::kPink},
    {"red", SpaceColor::kRed},
    {"orange", SpaceColor::kOrange},
    {"yellow", SpaceColor::kYellow},
    {"green", SpaceColor::kGreen},
});

// A space's icon, which the switcher shows instead of the name's first
// letter: none, an emoji, or a symbol of Compass's set (named as the sidebar
// page's SPACE_SYMBOLS, resources/sidebar/space_icons.ts). See
// docs/design/spaces.md, "Space icons".
class SpaceIcon {
 public:
  enum class Kind { kNone, kEmoji, kSymbol };

  // The longest emoji and symbol name accepted, in bytes. Emoji sequences
  // (families, flags with tags) take up to ~35.
  static constexpr size_t kMaxEmojiBytes = 64;
  static constexpr size_t kMaxSymbolBytes = 32;

  // None.
  SpaceIcon();
  // nullopt unless |emoji| is non-empty UTF-8 of at most kMaxEmojiBytes
  // without whitespace or control characters. Whether it's one emoji is the
  // page's to check.
  static std::optional<SpaceIcon> Emoji(std::string_view emoji);
  // nullopt unless |name| is 1-kMaxSymbolBytes of [a-z0-9-].
  static std::optional<SpaceIcon> Symbol(std::string_view name);

  SpaceIcon(const SpaceIcon&);
  SpaceIcon& operator=(const SpaceIcon&);
  ~SpaceIcon();

  Kind kind() const { return kind_; }
  bool is_none() const { return kind_ == Kind::kNone; }
  // The emoji or the symbol's name; empty for none.
  const std::string& value() const { return value_; }

  friend bool operator==(const SpaceIcon&, const SpaceIcon&) = default;

 private:
  SpaceIcon(Kind kind, std::string value);

  Kind kind_ = Kind::kNone;
  std::string value_;
};

// How long a Today entry may go unseen before it's archived, unless the space
// says otherwise. Arc's default.
inline constexpr base::TimeDelta kDefaultArchiveAfter = base::Hours(12);

// A space keeps its most recent archived entries; older ones are dropped.
inline constexpr size_t kMaxArchivedEntries = 100;

// A Today entry that was archived: a tab, or a split's panes. See
// docs/design/sidebar.md, "Today auto-archive".
struct ArchivedEntry {
  struct Tab {
    std::u16string title;
    GURL url;
    // The tab node's ID when it was archived. Sync identifies the archived
    // panes of a split by it.
    base::Uuid id;
  };

  ArchivedEntry();
  ArchivedEntry(const ArchivedEntry&);
  ArchivedEntry(ArchivedEntry&&);
  ArchivedEntry& operator=(const ArchivedEntry&);
  ArchivedEntry& operator=(ArchivedEntry&&);
  ~ArchivedEntry();

  // The archived node's ID: the tab's (also when it's the one pane of a
  // split worth keeping), or the split's. Saved, and the same on every synced
  // device, which sees archiving as a move of that entry (docs/design/sync.md,
  // "The archive").
  base::Uuid id = base::Uuid::GenerateRandomV4();
  std::vector<Tab> tabs;
  base::Time archived_at;
};

// One entry of a live folder as a refresh delivers it (the plugin's result,
// already normalized by the runtime: docs/design/live-folders.md, "Result").
struct LiveItem {
  // The plugin's stable ID for the item. Items with an empty or repeated key
  // are skipped.
  std::string key;
  GURL url;
  std::u16string title;
};

// The sidebar tree of one space: a pinned section (folders, tabs and splits;
// folders nest) and a today section (tabs and splits). Every window shows the
// same SpaceModel. The model knows nothing about WebContents or windows.
//
// Invariants:
//  - Folders only live in the pinned section.
//  - Splits contain only tabs, at most kMaxSplitPanes of them.
//  - A split left with one pane is replaced by that pane, in its position.
//  - The two roots can't be moved or removed.
//  - A Today folder whose last child leaves (moved, removed, archived) is
//    removed too, and so on up its parents. One made empty stays until
//    something has been in it and left. Not while a KeepEmptyFolders is
//    alive: sync applies another device's changes in an order that can empty
//    a folder for a moment.
//  - Live folders only live in the pinned section, and hold only their
//    entries: tabs with a live_item_key(), which ReplaceLiveItems() adds,
//    orders and removes. Nothing else moves in, and entries aren't reordered
//    or split; an entry moved out loses its key and becomes an ordinary tab.
//
// A profile's Favorites are a model of their own kind: its pinned root holds
// only tabs, and its Today root nothing. See docs/design/command-bar.md,
// "Slice 4".
class SpaceModel {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnNodeAdded(const SidebarNode& node) {}
    virtual void OnNodeMoved(const SidebarNode& node,
                             const SidebarNode& old_parent,
                             size_t old_index) {}
    virtual void OnNodeChanged(const SidebarNode& node) {}
    // Called before |node| and its subtree are destroyed.
    virtual void OnNodeWillBeRemoved(const SidebarNode& node) {}
    // Called before |node| and its subtree move to another space (see
    // MoveToSpace()). The nodes aren't destroyed: they keep their IDs, and
    // their live tabs.
    virtual void OnNodeWillMoveToSpace(const SidebarNode& node) {}
    // When |node| was last shown changed.
    virtual void OnNodeLastActiveChanged(const SidebarNode& node) {}
    // The space's name, color or archive delay changed.
    virtual void OnSpaceChanged(const SpaceModel& space) {}
    // Entries were archived, restored or dropped from the archive.
    virtual void OnArchiveChanged(const SpaceModel& space) {}
  };

  enum class Kind {
    kSpace,
    kFavorites,
  };

  // While one is alive, no model removes an emptied Today folder (see the
  // invariants). UI thread only.
  class KeepEmptyFolders {
   public:
    KeepEmptyFolders();
    KeepEmptyFolders(const KeepEmptyFolders&) = delete;
    KeepEmptyFolders& operator=(const KeepEmptyFolders&) = delete;
    ~KeepEmptyFolders();
  };

  explicit SpaceModel(Kind kind = Kind::kSpace);
  SpaceModel(const SpaceModel&) = delete;
  SpaceModel& operator=(const SpaceModel&) = delete;
  ~SpaceModel();

  Kind kind() const { return kind_; }

  // Stable across sessions (it's saved), unlike node IDs.
  const base::Uuid& id() const { return id_; }
  // Only while loading: the space takes over a saved space's identity.
  void set_id(const base::Uuid& id) { id_ = id; }

  const std::u16string& name() const { return name_; }
  void SetName(std::u16string name);

  SpaceColor color() const { return color_; }
  void SetColor(SpaceColor color);

  // The colors the sidebar's gradient goes on to after color(), none for a
  // plain color; at most kMaxSpaceGradientColors - 1, none of them grey. A
  // grey space has none. See docs/design/spaces.md, "Gradients".
  const std::vector<SpaceColor>& gradient() const { return gradient_; }
  // Drops grey entries and those past the limit.
  void SetGradient(std::vector<SpaceColor> gradient);

  // How strong the tint is and how much grain it has, 0 to 1 (clamped; NaN
  // is ignored). See docs/design/spaces.md, "Intensity and grain".
  double intensity() const { return intensity_; }
  void SetIntensity(double intensity);
  double grain() const { return grain_; }
  void SetGrain(double grain);

  SpaceAppearance appearance() const { return appearance_; }
  void SetAppearance(SpaceAppearance appearance);

  const SpaceIcon& icon() const { return icon_; }
  void SetIcon(SpaceIcon icon);

  // How long a Today entry may go unseen before ArchiveIdleEntries() archives
  // it. Zero means never.
  base::TimeDelta archive_after() const { return archive_after_; }
  void SetArchiveAfter(base::TimeDelta archive_after);

  // Whether the space syncs to the chain's other devices. Off, it stays on
  // this device: nothing of it goes up, and turning it off removes it from
  // the others (docs/design/sync.md, "Spaces that don't sync"). Not synced
  // itself. Favorites are always synced.
  bool synced() const { return synced_; }
  void SetSynced(bool synced);

  SidebarNode* pinned_root() { return pinned_root_.get(); }
  const SidebarNode* pinned_root() const { return pinned_root_.get(); }
  SidebarNode* today_root() { return today_root_.get(); }
  const SidebarNode* today_root() const { return today_root_.get(); }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // |id| is for a node that keeps an identity it had (saved, or synced from
  // another device); it must not be in use. Without one, the node gets a
  // fresh ID.
  SidebarNode* AddFolder(SidebarNode* parent,
                         size_t index,
                         std::u16string title,
                         const base::Uuid& id = base::Uuid());
  SidebarNode* AddTab(SidebarNode* parent,
                      size_t index,
                      const GURL& url,
                      std::u16string title,
                      const base::Uuid& id = base::Uuid());
  // An empty split, for loading and sync, which then add its panes (with
  // AddTab() or Move()). Split() is how users make one. Callers restore the
  // split invariants once its panes are in.
  SidebarNode* AddSplit(SidebarNode* parent,
                        size_t index,
                        const base::Uuid& id = base::Uuid());

  // Whether a live folder may be added to |parent|: the pinned root, or a
  // plain folder in Pinned. Never in Favorites.
  bool CanAddLiveFolder(const SidebarNode* parent) const;

  // A live folder, empty until ReplaceLiveItems() fills it. |parent| must
  // pass CanAddLiveFolder(); |id| is as for AddFolder().
  SidebarNode* AddLiveFolder(SidebarNode* parent,
                             size_t index,
                             std::u16string title,
                             LiveSource source,
                             const base::Uuid& id = base::Uuid());

  // Changes a live folder's plugin or settings. Its entries stay until the
  // next refresh replaces them. Observers see OnNodeChanged().
  void SetLiveSource(SidebarNode* folder, LiveSource source);

  // Makes a live folder plain: its entries stay, as ordinary tabs.
  // Observers see OnNodeChanged() for each entry, then the folder.
  void ConvertToPlainFolder(SidebarNode* folder);

  // Makes |folder|'s entries those of |items|, in their order, matched to
  // the entries there by key: a match keeps its node (its ID, its live tab)
  // and takes the item's URL and title as its saved page, and as its page
  // too unless |is_open| says its tab is open (then the tab keeps showing
  // what it shows). New items are added, unseen unless this is the folder's
  // first refresh. Entries no item matches are removed, unless their tab is
  // open: those stay, after the others, until a refresh finds them closed.
  // Records |now| as the folder's live_refreshed().
  void ReplaceLiveItems(SidebarNode* folder,
                        const std::vector<LiveItem>& items,
                        base::Time now,
                        base::FunctionRef<bool(const SidebarNode&)> is_open);

  // Only while loading: adds a saved entry to |folder|, which a saved
  // refresh time goes with (SetLiveRefreshed()).
  SidebarNode* AddLiveItem(SidebarNode* folder,
                           size_t index,
                           const LiveItem& item,
                           const base::Uuid& id = base::Uuid());
  void SetLiveRefreshed(SidebarNode* folder, base::Time refreshed);

  // Marks a live folder's entry seen or not. Observers see OnNodeChanged().
  void SetUnseen(SidebarNode* tab, bool unseen);
  // Marks every entry of |folder| seen.
  void MarkAllSeen(SidebarNode* folder);

  // Whether |node| may be moved into |new_parent|. Use it to validate drops.
  bool CanMove(const SidebarNode* node, const SidebarNode* new_parent) const;

  // Moves |node| into |new_parent| at |index|, counted after |node| has left
  // its current parent.
  void Move(SidebarNode* node, SidebarNode* new_parent, size_t index);

  // Whether |node| may move to |target| space, into |new_parent| (one of
  // |target|'s nodes).
  bool CanMoveToSpace(const SidebarNode* node,
                      const SpaceModel& target,
                      const SidebarNode* new_parent) const;

  // Moves |node| and its subtree into |target|'s |new_parent| at |index|.
  // This model's observers see OnNodeWillMoveToSpace(), |target|'s see
  // OnNodeAdded().
  void MoveToSpace(SidebarNode* node,
                   SpaceModel& target,
                   SidebarNode* new_parent,
                   size_t index);

  // Whether |tab| can be shown side by side with |target|.
  bool CanSplit(const SidebarNode* target, const SidebarNode* tab) const;

  // Shows |tab| next to |target|. If |target| is a standalone tab, both
  // become panes of a new split in |target|'s position. If |target| is a split
  // or one of its panes, |tab| becomes the split's last pane. Returns the
  // split.
  SidebarNode* Split(SidebarNode* target, SidebarNode* tab);

  // Puts |split|'s panes in its place, in order, as standalone tabs.
  void Unsplit(SidebarNode* split);

  // Removes |node| and its subtree.
  void Remove(SidebarNode* node);

  void SetTitle(SidebarNode* node, std::u16string title);
  // Names a tab; empty goes back to its page's title.
  void SetCustomTitle(SidebarNode* tab, std::u16string title);
  void SetUrl(SidebarNode* tab, const GURL& url);
  // Sets the page a pinned tab or favorite goes back to. Observers see
  // OnNodeChanged(). Nodes outside Pinned (and Favorites) keep none: moving
  // a tab there sets it to the tab's page, moving it out clears it.
  void SetSavedUrl(SidebarNode* tab, const GURL& url, std::u16string title);
  void SetLastActive(SidebarNode* tab, base::Time time);
  // Observers see OnNodeChanged().
  void SetCollapsed(SidebarNode* folder, bool collapsed);
  // Observers see OnNodeChanged(). Doesn't touch a live tab: LiveTabRegistry
  // and SpaceService keep the two in step.
  void SetMuted(SidebarNode* tab, bool muted);

  // Archived Today entries, most recent first.
  const std::vector<ArchivedEntry>& archive() const { return archive_; }

  // Moves |entry|, a Today entry (a tab or split, maybe in a folder), to the
  // archive: its tabs worth restoring (see IsRestorableURL()) are recorded,
  // then it's removed, which closes its live tabs. An entry with nothing
  // worth restoring is only removed.
  void Archive(SidebarNode* entry, base::Time now);

  // Archives the Today entries (in folders too; a folder emptied closes)
  // none of whose tabs has been shown for
  // archive_after(), as of |now|, except those |keep| returns true for (one
  // shown in a window right now, say). Tabs with no last-active time get
  // |now|, so they're archived a full delay later. Returns how many entries
  // were archived.
  size_t ArchiveIdleEntries(
      base::Time now,
      base::FunctionRef<bool(const SidebarNode&)> keep);

  // Puts the archived entry |id| back into Today at |index|, unloaded, and
  // returns it (a tab, or a split), or null if there's no such entry. The
  // restored nodes get fresh IDs.
  SidebarNode* Unarchive(const base::Uuid& id, size_t index);

  // The archived entry |id|, or null.
  const ArchivedEntry* FindArchived(const base::Uuid& id) const;

  // Adds |entry| to the archive in the place its time gives it (newest
  // first), replacing the entry with its ID if there's one. For sync, which
  // applies another device's archiving. An entry with no tabs is dropped.
  void PutArchived(ArchivedEntry entry);

  // Drops the archived entry |id|.
  void RemoveArchived(const base::Uuid& id);
  void ClearArchive();

  // Only while loading: appends |entry|, which is older than those already
  // there.
  void AddArchived(ArchivedEntry entry);

  SidebarNode* FindById(const base::Uuid& id);

  // Whether |node| is in this space's tree.
  bool Contains(const SidebarNode* node) const;

 private:
  bool CanContain(const SidebarNode* parent, SidebarNode::Type type) const;

  // Move() without its check, for ReplaceLiveItems(), which reorders entries
  // users can't.
  void MoveNode(SidebarNode* node, SidebarNode* new_parent, size_t index);

  // Clears the live item keys (and unseen) of the tabs in |node|'s subtree
  // that aren't in a live folder. Observers aren't told, as for
  // UpdateSavedUrls().
  void UpdateLiveItems(SidebarNode* node);

  void NotifyArchiveChanged();

  // Restores the split invariants after |split| lost a pane.
  void NormalizeSplit(SidebarNode* split);

  // Removes |parent| if it's a Today folder left empty.
  void CloseIfEmptyTodayFolder(SidebarNode* parent);

  // Gives the tabs of |node|'s subtree a saved URL if it's in Pinned and
  // they have none, or clears theirs if it isn't. Observers aren't told: it
  // happens as the subtree is added or moved, which they hear of.
  void UpdateSavedUrls(SidebarNode* node);

  SidebarNode* AddNode(SidebarNode* parent,
                       size_t index,
                       std::unique_ptr<SidebarNode> node);

  const Kind kind_;
  base::Uuid id_;
  std::u16string name_;
  SpaceColor color_ = SpaceColor::kGrey;
  std::vector<SpaceColor> gradient_;
  double intensity_ = kDefaultSpaceIntensity;
  double grain_ = 0;
  SpaceAppearance appearance_ = SpaceAppearance::kAuto;
  SpaceIcon icon_;
  base::TimeDelta archive_after_ = kDefaultArchiveAfter;
  bool synced_ = true;
  std::vector<ArchivedEntry> archive_;
  std::unique_ptr<SidebarNode> pinned_root_;
  std::unique_ptr<SidebarNode> today_root_;
  base::ObserverList<Observer> observers_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_SPACE_MODEL_H_
