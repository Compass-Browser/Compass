// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_SPACES_ANDROID_H_
#define COMPASS_BROWSER_ANDROID_SPACES_ANDROID_H_

#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"

class GURL;
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
  };

  // Creates it on first use; it loads the profile's spaces then.
  static SpacesAndroid* Get(Profile* profile);

  explicit SpacesAndroid(Profile* profile);
  SpacesAndroid(const SpacesAndroid&) = delete;
  SpacesAndroid& operator=(const SpacesAndroid&) = delete;
  ~SpacesAndroid() override;

  SpaceList& list() { return list_; }
  bool loaded() const { return loaded_; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // What the sidebar shows, as JSON: the spaces (id, name, tint, emoji), the
  // active one, its pinned and Today trees and the favorites; each node has
  // its id, type, title, url, "tab" (its tab, or -1) and children.
  std::string Snapshot() const;

  // Tabs, from the shell's TabModel observer. |restored| tabs come back from
  // the last session: they take the Today entry with their URL that has no
  // tab yet, if there is one, instead of adding one.
  void TabOpened(int tab_id, const GURL& url, const std::u16string& title,
                 bool restored);
  // A tab the shell opened from |node_id| (a pinned tab, favorite or Today
  // entry with no tab).
  void TabOpenedForNode(int tab_id, const base::Uuid& node_id);
  void TabUpdated(int tab_id, const GURL& url, const std::u16string& title);
  void TabSelected(int tab_id);
  // Today entries go when their tab closes; pinned tabs and favorites stay,
  // unloaded, back at their pinned URL.
  void TabClosed(int tab_id);

  // Moves |tab_id|'s entry to the end of its space's pinned tree, or of the
  // favorites; the tab stays open on it.
  void Pin(int tab_id, bool favorite);

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
  // Tells observers, once per task however many changes it made.
  void Changed();
  void NotifyChanged();
  SpaceModel* SpaceOf(const SidebarNode* node);

  SpaceList list_;
  std::unique_ptr<SidebarStore> store_;
  bool loaded_ = false;
  // Tab ID -> node it shows.
  std::map<int, base::Uuid> tabs_;
  bool notify_pending_ = false;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<SpacesAndroid> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_SPACES_ANDROID_H_
