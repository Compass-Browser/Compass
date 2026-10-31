// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_SPACE_MODEL_H_
#define COMPASS_BROWSER_SIDEBAR_SPACE_MODEL_H_

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

// Matches upstream split tabs (MultiContentsView). See ARCHITECTURE.md §9.
inline constexpr size_t kMaxSplitPanes = 2;

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
  };

  ArchivedEntry();
  ArchivedEntry(const ArchivedEntry&);
  ArchivedEntry(ArchivedEntry&&);
  ArchivedEntry& operator=(const ArchivedEntry&);
  ArchivedEntry& operator=(ArchivedEntry&&);
  ~ArchivedEntry();

  // Like node IDs, only stable for a session.
  base::Uuid id = base::Uuid::GenerateRandomV4();
  std::vector<Tab> tabs;
  base::Time archived_at;
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

  // How long a Today entry may go unseen before ArchiveIdleEntries() archives
  // it. Zero means never.
  base::TimeDelta archive_after() const { return archive_after_; }
  void SetArchiveAfter(base::TimeDelta archive_after);

  SidebarNode* pinned_root() { return pinned_root_.get(); }
  const SidebarNode* pinned_root() const { return pinned_root_.get(); }
  SidebarNode* today_root() { return today_root_.get(); }
  const SidebarNode* today_root() const { return today_root_.get(); }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  SidebarNode* AddFolder(SidebarNode* parent,
                         size_t index,
                         std::u16string title);
  SidebarNode* AddTab(SidebarNode* parent,
                      size_t index,
                      const GURL& url,
                      std::u16string title);

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
  void SetLastActive(SidebarNode* tab, base::Time time);
  // Observers see OnNodeChanged().
  void SetCollapsed(SidebarNode* folder, bool collapsed);

  // Archived Today entries, most recent first.
  const std::vector<ArchivedEntry>& archive() const { return archive_; }

  // Moves |entry|, a Today entry (a tab or split, maybe in a folder), to the
  // archive: its tabs worth restoring (see IsRestorableURL()) are recorded,
  // then it's removed, which closes its live tabs. An entry with nothing
  // worth restoring is only removed.
  void Archive(SidebarNode* entry, base::Time now);

  // Archives the Today entries (in folders too; folders stay, even emptied)
  // none of whose tabs has been shown for
  // archive_after(), as of |now|, except those |keep| returns true for (one
  // shown in a window right now, say). Tabs with no last-active time get
  // |now|, so they're archived a full delay later. Returns how many entries
  // were archived.
  size_t ArchiveIdleEntries(
      base::Time now,
      base::FunctionRef<bool(const SidebarNode&)> keep);

  // Puts the archived entry |id| back into Today at |index|, unloaded, and
  // returns it (a tab, or a split), or null if there's no such entry.
  SidebarNode* Unarchive(const base::Uuid& id, size_t index);

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

  void NotifyArchiveChanged();

  // Restores the split invariants after |split| lost a pane.
  void NormalizeSplit(SidebarNode* split);

  SidebarNode* AddNode(SidebarNode* parent,
                       size_t index,
                       std::unique_ptr<SidebarNode> node);

  const Kind kind_;
  base::Uuid id_;
  std::u16string name_;
  SpaceColor color_ = SpaceColor::kGrey;
  base::TimeDelta archive_after_ = kDefaultArchiveAfter;
  std::vector<ArchivedEntry> archive_;
  std::unique_ptr<SidebarNode> pinned_root_;
  std::unique_ptr<SidebarNode> today_root_;
  base::ObserverList<Observer> observers_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_SPACE_MODEL_H_
