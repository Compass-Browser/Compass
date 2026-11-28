// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_SIDEBAR_NODE_H_
#define COMPASS_BROWSER_SIDEBAR_SIDEBAR_NODE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "base/values.h"
#include "url/gurl.h"

namespace compass {

// Where a live folder's entries come from: a plugin, and the folder's
// settings for it. The part of a live folder that syncs; the entries don't
// (docs/design/live-folders.md).
struct LiveSource {
  LiveSource();
  LiveSource(std::string plugin_id,
             base::DictValue config,
             GURL plugin_url = GURL(),
             std::string plugin_sha256 = std::string());
  LiveSource(const LiveSource&);
  LiveSource& operator=(const LiveSource&);
  LiveSource(LiveSource&&);
  LiveSource& operator=(LiveSource&&);
  ~LiveSource();

  friend bool operator==(const LiveSource&, const LiveSource&) = default;

  // The plugin's manifest ID, e.g. "rss". Never empty.
  std::string plugin_id;
  // The values of the plugin's config fields, by key.
  base::DictValue config;
  // Where an installed plugin came from, and the SHA-256 (lowercase hex) of
  // what was installed; empty for a built-in.
  GURL plugin_url;
  std::string plugin_sha256;
};

// One node in a space's sidebar tree. Nodes are created and changed only
// through SpaceModel, which enforces the tree's invariants.
class SidebarNode {
 public:
  enum class Type {
    // The pinned and today sections. Each space has exactly one of each.
    kRoot,
    kFolder,
    kTab,
    // One sidebar entry that shows its child tabs (panes) side by side.
    kSplit,
  };

  // Where a folder's name came from (docs/design/ai-sidebar.md, "Names for
  // link folders"). Saved and synced; a folder without one (made by hand, by
  // an older build or another client) is the user's.
  enum class NameSource {
    // Named after the page whose links it gathers (a link folder).
    kOpener,
    // Named by Claude.
    kClaude,
    // Named, or renamed, by the user: Claude never names it on its own.
    kUser,
  };

  // |id| is for nodes that keep an identity they had before (saved, or from
  // another device); an invalid one gets a fresh ID.
  SidebarNode(Type type,
              std::u16string title,
              GURL url,
              const base::Uuid& id = base::Uuid());
  SidebarNode(const SidebarNode&) = delete;
  SidebarNode& operator=(const SidebarNode&) = delete;
  ~SidebarNode();

  // Stable: saved with the node, and the same on every synced device.
  const base::Uuid& id() const { return id_; }
  Type type() const { return type_; }
  // A tab's page title, a folder's name.
  const std::u16string& title() const { return title_; }
  // The name the user gave a tab, or empty. Kept while its page's title
  // changes, and saved.
  const std::u16string& custom_title() const { return custom_title_; }
  // The custom title, else the title: what the command bar, Claude's
  // descriptions and the tab strip show.
  const std::u16string& display_title() const {
    return custom_title_.empty() ? title_ : custom_title_;
  }
  // A shorter title Claude gave the tab's title() ("Shorter Tab Titles",
  // docs/design/ai-sidebar.md), or empty. Display only: the sidebar's row
  // shows it when there's no custom title; the hover card, the window title
  // and the command bar keep title(). Not saved or synced; cleared whenever
  // title() changes.
  const std::u16string& short_title() const { return short_title_; }
  // Where a folder's name came from; kUser for other nodes.
  NameSource name_source() const { return name_source_; }
  // A folder's name before Claude named it, which Undo Name goes back to;
  // empty while the name isn't Claude's, or isn't known (Claude named it on
  // another device). Saved, not synced.
  const std::u16string& name_before_claude() const {
    return name_before_claude_;
  }
  // Whether this device made the folder for a page's links (in this
  // session): only that device asks Claude to name it. Not saved.
  bool link_folder_here() const { return link_folder_here_; }
  // The page a tab shows: follows its live tab's navigations.
  const GURL& url() const { return url_; }
  // A pinned tab's or favorite's own page, which its tab goes back to when
  // it closes (Arc's "pinned URL"), and its title. Set when the tab is
  // pinned or made a favorite, and saved; empty elsewhere
  // (docs/design/sidebar.md, "Pinned URL").
  const GURL& saved_url() const { return saved_url_; }
  const std::u16string& saved_title() const { return saved_title_; }
  // When a tab node was last shown in a window, or null if that's unknown
  // (see SpaceModel::ArchiveIdleEntries()). Unset for other nodes.
  base::Time last_active() const { return last_active_; }
  // Whether a folder shows only its own row. Shared by every window, and
  // saved. False for other nodes.
  bool collapsed() const { return collapsed_; }
  // Whether a folder is a tab tree (docs/design/sidebar.md, "Tree-style
  // tabs"): drawn as its first entry, a tab or a split, with the rest of its
  // entries (the tabs opened from it) under that one, and no row of its own.
  // Saved and synced; a client that doesn't know it shows a plain folder.
  // False for other nodes.
  bool tab_tree() const { return tab_tree_; }
  // Whether a tab is muted: its live tab is, and it will be when it next
  // opens. Follows the live tab's mute state, and saved. False for other
  // nodes (docs/design/sidebar.md, "Sound").
  bool muted() const { return muted_; }
  // A live folder's source; nullopt for other nodes. Saved.
  const std::optional<LiveSource>& live_source() const { return live_source_; }
  bool is_live_folder() const { return live_source_.has_value(); }
  // When a live folder's entries were last replaced by a refresh, or null.
  // Saved, not synced.
  base::Time live_refreshed() const { return live_refreshed_; }
  // A live folder's entry: the plugin's ID for the item it shows. Empty for
  // every other node, including a tab dragged out of its live folder.
  // Saved, not synced.
  const std::string& live_item_key() const { return live_item_key_; }
  bool is_live_item() const { return !live_item_key_.empty(); }
  // Whether a live folder's entry came in with a refresh and hasn't been
  // seen. False for other nodes. Saved.
  bool unseen() const { return unseen_; }
  SidebarNode* parent() const { return parent_.get(); }
  const std::vector<std::unique_ptr<SidebarNode>>& children() const {
    return children_;
  }

  // Returns the position of |child|, which must be a child of this node.
  size_t GetIndexOf(const SidebarNode* child) const;

  // Whether |node| is this node or one of its descendants.
  bool Contains(const SidebarNode* node) const;

 private:
  friend class SpaceModel;

  SidebarNode* Add(std::unique_ptr<SidebarNode> child, size_t index);
  std::unique_ptr<SidebarNode> Remove(const SidebarNode* child);

  const base::Uuid id_;
  const Type type_;
  std::u16string title_;
  std::u16string custom_title_;
  std::u16string short_title_;
  GURL url_;
  GURL saved_url_;
  std::u16string saved_title_;
  base::Time last_active_;
  bool collapsed_ = false;
  bool tab_tree_ = false;
  bool muted_ = false;
  std::optional<LiveSource> live_source_;
  base::Time live_refreshed_;
  std::string live_item_key_;
  bool unseen_ = false;
  NameSource name_source_ = NameSource::kUser;
  std::u16string name_before_claude_;
  bool link_folder_here_ = false;
  raw_ptr<SidebarNode> parent_ = nullptr;
  std::vector<std::unique_ptr<SidebarNode>> children_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_SIDEBAR_NODE_H_
