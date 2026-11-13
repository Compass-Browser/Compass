// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_SPACES_ANDROID_H_
#define COMPASS_BROWSER_ANDROID_SPACES_ANDROID_H_

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/supports_user_data.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "url/gurl.h"

class Profile;

namespace compass {

class SidebarStore;

// A profile's spaces on Android (docs/design/android.md, C2): the desktop's
// model (SpaceList, saved by SidebarStore to the same "Compass Sidebar.json")
// without its windows. Android's tabs are Chrome's TabModel, in Java; the
// shell reports them here (TabOpened() and so on), and this keeps a tab ->
// node map: an open tab is a Today entry, or a pinned tab or favorite it was
// opened from.
class SpacesAndroid : public base::SupportsUserData::Data,
                      public SpaceList::Observer,
                      public SpaceModel::Observer {
 public:
  // Something changed in any space, the favorites, or the tab map.
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnSpacesChanged() = 0;
    // The archiver archived the entries of these open tabs, which should
    // close now (without undo): they have no entry any more.
    virtual void OnTabsArchived(const std::vector<int>& tab_ids) {}
  };

  // What sync keeps on the spaces (compass_spaces_sync_android.cc).
  class Attachment {
   public:
    virtual ~Attachment() = default;
  };

  // Creates it on first use; it loads the profile's spaces then.
  static SpacesAndroid* Get(Profile* profile);
  static SpacesAndroid* GetIfExists(Profile* profile);

  explicit SpacesAndroid(Profile* profile);
  SpacesAndroid(const SpacesAndroid&) = delete;
  SpacesAndroid& operator=(const SpacesAndroid&) = delete;
  ~SpacesAndroid() override;

  SpaceList& list() { return list_; }
  bool loaded() const { return loaded_; }
  // Runs |callback| once the saved spaces are in (now, if they are).
  void RunWhenLoaded(base::OnceClosure callback);

  Attachment* sync_attachment() { return sync_attachment_.get(); }
  void SetSyncAttachment(std::unique_ptr<Attachment> attachment) {
    sync_attachment_ = std::move(attachment);
  }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // What a window's sidebar shows, as JSON: the spaces (id, name, tint,
  // emoji), the one the window shows ("active": |space_id|, or the list's
  // active space if there's no such space), its pinned and Today trees and
  // the favorites; each node has its id, type, title, url, "tab" (its tab, or
  // -1), "saved_url" (a pinned tab's or favorite's pinned URL) and children.
  // The shown space's archive comes as "archive" (id, title, url and
  // "archived_at", in milliseconds since the epoch; newest first), with its
  // "archive_after_hours" (0: never).
  std::string Snapshot(const base::Uuid& space_id) const;
  // |space_id|'s space, or the list's active one if there's no such space.
  // Each window shows a space of its own; the list's active space is the
  // last one a window switched to, which new windows start on.
  SpaceModel& SpaceOrActive(const base::Uuid& space_id) const;

  // Tabs, from the shell's TabModel observer. |restored| tabs come back from
  // the last session: they take the Today entry with their URL that has no
  // tab yet, if there is one, instead of adding one. A tab on the new tab
  // page (the start page) gets no entry until it goes somewhere, as the
  // desktop's empty tab (command-bar.md, "No new tab page"). A new entry
  // goes in |space_id|'s Today (the window's space; SpaceOrActive()). A tab
  // that moved here from another window keeps its entry.
  void TabOpened(int tab_id,
                 const GURL& url,
                 const std::u16string& title,
                 bool restored,
                 const base::Uuid& space_id = base::Uuid());
  // A tab the shell opened from |node_id| (a pinned tab, favorite or Today
  // entry with no tab).
  void TabOpenedForNode(int tab_id, const base::Uuid& node_id);
  void TabUpdated(int tab_id, const GURL& url, const std::u16string& title);
  // |window| (any pointer that's the window's) now shows |tab_id|. Stamps
  // the entry it shows, and the one the window showed before, as last
  // active: the archiver's clock starts as an entry leaves the screen
  // (docs/design/sidebar.md, "Today auto-archive").
  void TabSelected(int tab_id, const void* window = nullptr);
  // |window| is gone: what it showed may be archived.
  void WindowClosed(const void* window);
  // A Today entry goes to its space's archive when its tab closes
  // (docs/design/android.md, P1); pinned tabs and favorites stay, unloaded,
  // back at their pinned URL.
  void TabClosed(int tab_id);
  // A closed tab came back (undo): its entry comes back out of the archive,
  // at the top of Today, or a new Today entry is made for it.
  void TabClosureUndone(int tab_id,
                        const GURL& url,
                        const std::u16string& title,
                        const base::Uuid& space_id = base::Uuid());

  // Archives the Today entry |node_id| (a tab or split). One with a tab
  // should be archived by closing the tab instead; if it has one anyway, the
  // tab is let go (observers see OnTabsArchived()).
  void ArchiveNode(const base::Uuid& node_id);
  // Puts the archived entry |entry_id| (of any space) back at the top of its
  // space's Today, unloaded. Returns the tab node to open (a split's first
  // pane), or null.
  SidebarNode* RestoreArchived(const base::Uuid& entry_id);
  void RemoveArchived(const base::Uuid& entry_id);
  // |hours| 0: never.
  void SetArchiveAfter(const base::Uuid& space_id, int hours);
  // Archives idle Today entries now, as the timer does every 10 minutes
  // (after 30 s for the first time).
  void ArchiveIdleEntries();

  // Makes the pinned tab's or favorite's current page its pinned URL.
  void UseCurrentPageAsSavedUrl(const base::Uuid& node_id);

  // Moves |tab_id|'s entry to the end of its space's pinned tree, or of the
  // favorites; the tab stays open on it.
  void Pin(int tab_id, bool favorite);

  // Where MoveNode() puts an entry.
  enum class Section { kPinned, kToday, kFavorites };
  // Moves |node_id| (with its tab, if it has one) to |space_id|'s pinned
  // tree (at the end) or Today (at the top), or to the end of the favorites
  // (|space_id| unused).
  void MoveNode(const base::Uuid& node_id,
                const base::Uuid& space_id,
                Section section);

  // Drag and drop (docs/design/android.md, P5): moves |node_id| (with its
  // tab) into |parent_id| (a folder, or a pinned or Today root), before
  // |before_id| (one of its children), or at its end if that's invalid.
  // Across spaces too. Returns whether it moved (CanMove() decides).
  bool MoveNodeTo(const base::Uuid& node_id,
                  const base::Uuid& parent_id,
                  const base::Uuid& before_id);

  // Hands |from|'s tab, if it has one, to |to|, if it has none (sync
  // replaced an entry with another).
  void TransferTab(const base::Uuid& from, const base::Uuid& to);
  // The tab on screen, or -1.
  int current_tab() const { return current_tab_; }

  // The tab |node_id| is open in, or -1.
  int TabFor(const base::Uuid& node_id) const;
  SidebarNode* FindNode(const base::Uuid& id);

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceMoved(const SpaceModel& space) override;
  void OnActiveSpaceChanged() override;

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeChanged(const SidebarNode& node) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;
  void OnSpaceChanged(const SpaceModel& space) override;
  void OnArchiveChanged(const SpaceModel& space) override;

 private:
  base::ListValue SnapshotSection(const SidebarNode& root) const;
  void OnLoaded();
  // Whether |entry| shows a tab that a window shows.
  bool IsShown(const SidebarNode& entry);
  // Whether a tab of |entry| (a tab, or a split's panes) plays sound.
  bool IsPlayingSound(const SidebarNode& entry) const;
  // Tells observers, once per task however many changes it made.
  void Changed();
  void NotifyChanged();
  SpaceModel* SpaceOf(const SidebarNode* node);

  SpaceList list_;
  std::unique_ptr<SidebarStore> store_;
  bool loaded_ = false;
  // Tab ID -> node it shows.
  std::map<int, base::Uuid> tabs_;
  // Tabs on the new tab page, with no entry yet.
  std::set<int> empty_tabs_;
  // Tabs reported before the saved spaces were in: they're matched to saved
  // entries once they are (OnLoaded()).
  struct PendingTab {
    int id;
    GURL url;
    std::u16string title;
    base::Uuid space_id;
  };
  std::vector<PendingTab> pending_tabs_;
  // An entry with |url| and no tab: a favorite, then a pinned tab, then a
  // Today tab, in any space.
  SidebarNode* FindUnopenedEntry(const GURL& url);
  int current_tab_ = -1;
  // The tab each window shows.
  std::map<const void*, int> shown_tabs_;
  // Tabs whose Today entry went to the archive as they closed, with the
  // archived entry's ID, for undo. The most recent few.
  std::map<int, base::Uuid> archived_closed_tabs_;
  base::OneShotTimer first_archive_timer_;
  base::RepeatingTimer archive_timer_;
  std::vector<base::OnceClosure> on_loaded_;
  bool notify_pending_ = false;
  // Destroyed first: sync's holder observes the list.
  std::unique_ptr<Attachment> sync_attachment_;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<SpacesAndroid> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_SPACES_ANDROID_H_
