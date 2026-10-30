// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
#define COMPASS_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/base/mojom/window_show_state.mojom-forward.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;
class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace gfx {
class Rect;
}  // namespace gfx

namespace compass {

// Compass's per-window state: the space the window shows, and the bridge
// between the window's tab strip and that space. Owned by upstream's
// EmbedderBrowserWindowFeatures (see patches/) and created before the window's
// views. The profile's parking window gets these too, without a bridge: its
// tabs keep the nodes they had. See docs/design/spaces.md.
class WindowFeatures : public SpaceList::Observer {
 public:
  DECLARE_USER_DATA(WindowFeatures);

  class Observer : public base::CheckedObserver {
   public:
    // The window now shows another space, with a new bridge.
    virtual void OnSpaceSwitched() {}
    // A swipe in the sidebar started, moved or stopped pulling in another
    // space (see SetSpacePreview()).
    virtual void OnSpacePreviewChanged() {}
    // The window is swapping to another profile's space and is about to
    // empty and close (SwitchWindowToSpace()). Its bridge and space can go
    // from now on.
    virtual void OnSwappedAway() {}
  };

  explicit WindowFeatures(BrowserWindowInterface* browser);
  WindowFeatures(const WindowFeatures&) = delete;
  WindowFeatures& operator=(const WindowFeatures&) = delete;
  ~WindowFeatures() override;

  static WindowFeatures* From(BrowserWindowInterface* browser);

  SpaceService& service() { return *service_; }
  // The space this window shows. Shared with the profile's other windows.
  SpaceModel& space_model() { return *space_; }
  LiveTabRegistry& registry() { return service_->registry(); }

  bool is_parking_window() const { return is_parking_window_; }
  // Set during SwitchToSpace(), while the window has no bridge for a moment,
  // and for good once the window swapped away.
  bool is_switching() const { return switching_ || swapped_away_; }
  // The window is swapping to another profile's space
  // (SwitchWindowToSpace()): it's about to empty and close.
  bool swapped_away() const { return swapped_away_; }
  // Not for the parking window.
  TabStripBridge& tab_strip_bridge() {
    CHECK(bridge_);
    return *bridge_;
  }

  // Shows |space| in this window. The live tabs of the space it showed go to
  // another window showing that space, or to the parking window; those of
  // |space| come back from the parking window. See docs/design/spaces.md,
  // "Switching, step by step".
  void SwitchToSpace(SpaceModel& space);

  // For a window just opened on its space, with no tabs, by a swap (see
  // SwitchWindowToSpace()): steps 3 to 5 of a switch. The space's parked tabs
  // come in, and the entry it showed last is shown.
  void EnterSpace();
  // The other half of a swap, in two steps. BeginSwap(): the window is
  // swapping away; its page stops taking requests (OnSwappedAway()), and it
  // can't swap again. It shows its tabs until the new window has taken its
  // place. LeaveForSwap(): every tab leaves, favorites' too, as in step 6 of a
  // switch, and the emptied window closes.
  void BeginSwap();
  void LeaveForSwap();

  // A swipe in the sidebar is pulling in |space|, |progress| (0 to 1) of the
  // way, before it switches; null ends it. SwitchToSpace() ends it too,
  // without OnSpacePreviewChanged(). See docs/design/spaces.md, "Swiping".
  void SetSpacePreview(SpaceModel* space, double progress);
  // The space being pulled in, or null; gone spaces aren't.
  SpaceModel* preview_space();
  double preview_progress() const { return preview_progress_; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // SpaceList::Observer:
  void OnSpaceWillBeRemoved(SpaceModel& space) override;

 private:
  void OnActiveTabChanged(BrowserWindowInterface* browser);
  void OnWindowBecameActive(BrowserWindowInterface* browser);

  // Steps 3 to 5 of a switch, for the space the window now shows.
  void EnterCurrentSpace();
  // Step 6 of a switch: |leaving| go to another window showing their space
  // (favorites' to the parking window), else to the parking window; blank
  // Today tabs and placeholders close. |keep_one| keeps the strip from
  // emptying.
  void MoveTabsOut(const std::vector<tabs::TabHandle>& leaving,
                   SpaceModel& leaving_space,
                   bool keep_one);

  // Another normal window of the profile that shows |space|, or null.
  BrowserWindowInterface* FindOtherWindowShowing(const SpaceModel& space);

  const raw_ptr<BrowserWindowInterface> browser_;
  // Outlives the window: it belongs to the profile.
  const raw_ptr<SpaceService> service_;
  raw_ptr<SpaceModel> space_;
  const bool is_parking_window_;
  bool switching_ = false;
  bool swapped_away_ = false;
  // By ID: the space can be deleted mid-swipe.
  base::Uuid preview_space_id_;
  double preview_progress_ = 0;
  // Null for the parking window.
  std::unique_ptr<TabStripBridge> bridge_;

  base::ObserverList<Observer> observers_;
  base::CallbackListSubscription active_tab_subscription_;
  base::CallbackListSubscription did_become_active_subscription_;

  // Makes From() work. Upstream's EmbedderBrowserWindowFeatures::From() can't
  // be used: it never registers itself.
  ui::ScopedUnownedUserData<WindowFeatures> scoped_data_holder_;
};

// The entry a window switching to |space| would show, as far as can be told
// beforehand (SwitchToSpace(), step 5): its last active entry if that has a
// tab, else its first entry with one, else the one it would open. Null when
// it would open a new tab. For the sidebar's previews.
SidebarNode* PredictShownEntry(SpaceService& service, SpaceModel& space);

// Moves |node|, an entry of one of |profile|'s spaces, to |target|: a pinned
// entry to the end of |target|'s Pinned, a Today entry to the end of its
// Today. Its live tabs go with it, out of windows showing the space it left,
// to a window showing |target| or to the parking window. A window whose
// active tab leaves shows another one. Entries that can't go there (a folder
// into Today) stay. See docs/design/spaces.md, "Moving entries".
void MoveEntryToSpace(Profile* profile, SidebarNode* node, SpaceModel& target);

// Moves |node|, an entry of one of |profile|'s spaces or one of its favorites,
// into |new_parent| at |index| in |target|: another space or the favorites.
// Into the favorites, its live tab stays where it is (docs/design/
// command-bar.md, "Slice 4"); into a space, its live tabs move as for
// MoveEntryToSpace(). Moves |target| can't take are ignored.
void MoveEntry(Profile* profile,
               SidebarNode* node,
               SpaceModel& target,
               SidebarNode* new_parent,
               size_t index);

// Deletes |space|, one of |profile|'s, unless it's the switcher's only space.
// Windows showing it switch to the space after it in the switcher (before it,
// if it's last), swapping if that's another profile's; then its tabs close
// and its entries go. A profile whose last space goes leaves the switcher.
// See docs/design/spaces.md, "Deleting a space".
void DeleteSpace(Profile* profile, SpaceModel& space);

// Assigns |space|, one of |from|'s, to |to|, as Arc does: a space of |to|
// takes its place in the switcher with its name, color, entries and archive.
// Its live tabs can't change profile: they close, their entries stay, and
// windows showing it swap to a window of |to| that loads the entry it showed.
// |from| leaves the switcher if that was its last space. See
// docs/design/spaces.md, "Assigning a profile".
void MoveSpaceToProfile(Profile* from, SpaceModel& space, Profile* to);

// MoveSpaceToProfile() for space |space_id| and the profile at
// |profile_path|, or a new profile if there's none, once that profile and its
// sidebar file have loaded. Does nothing if either is gone by then.
void AssignSpaceToProfile(const base::Uuid& space_id,
                          std::optional<base::FilePath> profile_path);

// Shows |space|, one of |profile|'s spaces, in |browser|'s window. A space of
// the window's profile rebinds the window (SwitchToSpace()). One of another
// profile swaps it: a window of that profile opens on |space| at the same
// bounds, and |browser| empties and closes. Returns the window now showing
// |space|, or null if it couldn't. See docs/design/spaces.md, slice 3.
BrowserWindowInterface* SwitchWindowToSpace(BrowserWindowInterface* browser,
                                            Profile* profile,
                                            SpaceModel& space);

// The spaces |browser|'s window can switch to, in the switcher's order: every
// listed profile's (SpaceDirectory), or only its profile's in an
// off-the-record window.
std::vector<SpaceDirectory::Entry> GetSwitchableSpaces(
    BrowserWindowInterface* browser);
// The one of those with |id|, or an empty entry.
SpaceDirectory::Entry FindSwitchableSpace(BrowserWindowInterface* browser,
                                          const base::Uuid& id);

// Opens the windows of swaps. Tests can't create real windows.
using SwapWindowFactory = base::RepeatingCallback<BrowserWindowInterface*(
    Profile*,
    const gfx::Rect& bounds,
    ui::mojom::WindowShowState show_state)>;
void SetSwapWindowFactoryForTesting(SwapWindowFactory factory);

// Whether the window being built is a swap's, which opens hidden and only
// shows once it has taken the old window's place.
bool IsCreatingSwapWindow();

// Puts |window|, which a swap just opened with its tabs, in |old|'s place:
// shows it without a flicker once it has painted, then runs |leave|, which
// empties |old|. Set by the views layer (docs/design/spaces.md, "Swapping
// the window"); without one, |window| shows at once.
using SwapPresenter =
    base::RepeatingCallback<void(BrowserWindowInterface* old,
                                 BrowserWindowInterface* window,
                                 base::OnceClosure leave)>;
void SetSwapPresenter(SwapPresenter presenter);

// Called when |browser|'s window is asked to close, before beforeunload
// handlers run. Hands its tabs to the most recently active other window of
// the profile that shows the same space, unless the app is quitting. Returns
// true if it did; the emptied window then closes on its own, and the caller
// must not go on closing it. See docs/design/sidebar.md, "Window sync".
bool HandOffTabsBeforeClose(BrowserWindowInterface* browser);

// Whether |contents| is a tab in its profile's parking window: alive, but in
// no window the user can see. Upstream's auto picture-in-picture counts a tab
// moved there as switched away from (see docs/design/picture-in-picture.md).
bool IsInParkingWindow(content::WebContents* contents);

// Shows |contents|, a tab in its profile's parking window, as a click on its
// entry would: the window the user was in last switches to the tab's space
// (another profile's too) and activates it. For pages and picture-in-picture
// windows asking to focus their tab (BrowserWebContentsDelegate::
// ActivateContents()), which would otherwise show the parking window. Returns
// false for any other tab. See docs/design/picture-in-picture.md, "Back to
// tab".
bool RevealParkedTab(content::WebContents* contents);

// Archives the Today entries of |profile|'s spaces that have gone unseen for
// their space's delay, as of |now| (see SpaceModel::ArchiveIdleEntries()).
// Entries shown in a window, or playing sound, stay. See
// docs/design/sidebar.md, "Today auto-archive".
void ArchiveIdleTodayEntries(Profile* profile, base::Time now);

}  // namespace compass

#endif  // COMPASS_BROWSER_WINDOW_SYNC_WINDOW_FEATURES_H_
