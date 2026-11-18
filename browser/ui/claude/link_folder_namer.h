// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_LINK_FOLDER_NAMER_H_
#define COMPASS_BROWSER_UI_CLAUDE_LINK_FOLDER_NAMER_H_

#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "compass/browser/claude/claude_quick.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/claude/sidebar_spaces.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace compass {


// Names link folders with Claude (docs/design/ai-sidebar.md, "Names for
// link folders"): a link folder this device made (TabStripBridge), in Today,
// whose name is still its opener's or Claude's, is named from its entries'
// titles and hosts when it reaches 3, 6 and 12 entries, 5 s after its last
// change. Only with "Name Link Folders" on (prefs::kNameLinkFolders, and
// Claude set up: ClaudeQuick); never off the record. Also "Suggest a Name"
// for any folder, on request, under the same switch.
class LinkFolderNamer : public KeyedService,
                        public SpaceList::Observer,
                        public SpaceModel::Observer {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Claude just named |folder_id|: the sidebar shows a sparkle.
    virtual void OnFolderNamed(const base::Uuid& folder_id) {}
  };

  // The entry counts that ask for a name.
  static constexpr size_t kThresholds[] = {3, 6, 12};
  static constexpr base::TimeDelta kDebounce = base::Seconds(5);
  // The most tabs a request lists.
  static constexpr size_t kMaxTabs = 20;

  LinkFolderNamer(Profile* profile, claude::ClaudeQuick* quick);
  LinkFolderNamer(const LinkFolderNamer&) = delete;
  LinkFolderNamer& operator=(const LinkFolderNamer&) = delete;
  ~LinkFolderNamer() override;

  // Null off the record and for guests.
  static LinkFolderNamer* Get(Profile* profile);

  // The user message for |folder|: its tabs' titles and hosts, a line each.
  static std::string DescribeFolder(const SidebarNode& folder);

  // Whether Claude may be asked at all ("Name Link Folders" on, and Claude
  // set up).
  bool IsEnabled() const;
  // Asks for a name for |folder_id| now, as the user did (Suggest a Name);
  // the name, if any, is Claude's, with Undo Name.
  void Suggest(const base::Uuid& folder_id);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Waits |debounce| instead of kDebounce after a folder's last change.
  void SetDebounceForTesting(base::TimeDelta debounce) {
    debounce_ = debounce;
  }
  // Whether it follows the spaces yet (they load after it's made).
  bool IsObservingForTesting() const { return list_observation_.IsObserving(); }

  // KeyedService:
  void Shutdown() override;

  // SpaceList::Observer:
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceWillBeRemoved(SpaceModel& space) override;

  // SpaceModel::Observer:
  void OnNodeAdded(const SidebarNode& node) override;
  void OnNodeMoved(const SidebarNode& node,
                   const SidebarNode& old_parent,
                   size_t old_index) override;
  void OnNodeChanged(const SidebarNode& node) override;
  void OnNodeWillBeRemoved(const SidebarNode& node) override;

 private:
  void OnSpacesLoaded();
  // The folder |id| and its space, or null.
  SidebarNode* FindFolder(const base::Uuid& id, SpaceModel** space);
  // Whether Claude may name |folder| on its own now.
  bool IsEligible(const SidebarNode& folder) const;
  // A change in |node|'s folder: (re)starts its wait if it reached a
  // threshold it wasn't named at, or is waiting.
  void OnFolderChanged(const SidebarNode* node);
  void OnWaited(const base::Uuid& folder_id);
  void Ask(const base::Uuid& folder_id, bool background);
  void OnName(const base::Uuid& folder_id,
              claude::ClaudeQuick::Result result);

  const raw_ptr<Profile> profile_;
  const raw_ptr<claude::ClaudeQuick> quick_;
  base::WeakPtr<SidebarSpaces> space_service_;
  base::TimeDelta debounce_ = kDebounce;
  // The highest threshold each folder was asked at.
  std::map<base::Uuid, size_t> asked_at_;
  // Folders waiting for their changes to settle.
  std::map<base::Uuid, std::unique_ptr<base::OneShotTimer>> waiting_;
  // Folders with a request on its way, and whether it's a background one.
  std::map<base::Uuid, bool> asking_;
  base::ScopedObservation<SpaceList, SpaceList::Observer> list_observation_{
      this};
  base::ScopedMultiSourceObservation<SpaceModel, SpaceModel::Observer>
      model_observations_{this};
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<LinkFolderNamer> weak_factory_{this};
};

class LinkFolderNamerFactory : public ProfileKeyedServiceFactory {
 public:
  static LinkFolderNamerFactory* GetInstance();
  static LinkFolderNamer* GetForProfile(Profile* profile);

 private:
  friend base::NoDestructor<LinkFolderNamerFactory>;

  LinkFolderNamerFactory();
  ~LinkFolderNamerFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_CLAUDE_LINK_FOLDER_NAMER_H_
