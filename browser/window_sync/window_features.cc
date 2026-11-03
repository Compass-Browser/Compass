// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/window_features.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/desktop_browser_window_capabilities.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_contents.h"
#include "compass/browser/sidebar/sidebar_serializer.h"
#include "compass/browser/window_sync/empty_tab.h"
#include "compass/browser/window_sync/sidebar_store.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/startup_space.h"
#include "ui/base/base_window.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The first tab entry under |node| (itself included), depth first.
SidebarNode* FindFirstTab(SidebarNode* node) {
  if (node->type() == SidebarNode::Type::kTab) {
    return node;
  }
  for (const auto& child : node->children()) {
    if (SidebarNode* tab = FindFirstTab(child.get())) {
      return tab;
    }
  }
  return nullptr;
}

// The first tab node under |node| (itself included) that has a tab.
SidebarNode* FindFirstLiveTab(SidebarNode* node, LiveTabRegistry& registry) {
  if (node->type() == SidebarNode::Type::kTab) {
    return registry.GetTab(node) ? node : nullptr;
  }
  for (const auto& child : node->children()) {
    if (SidebarNode* tab = FindFirstLiveTab(child.get(), registry)) {
      return tab;
    }
  }
  return nullptr;
}

// Whether |browser| is a window the user can see tabs in: not the parking
// window, and not closing.
bool IsUsableNormalWindow(BrowserWindowInterface* browser) {
  WindowFeatures* features = WindowFeatures::From(browser);
  return browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
         !browser->capabilities()->IsAttemptingToCloseBrowser() && features &&
         !features->is_parking_window() && !features->swapped_away();
}

// A usable normal window of |profile| other than |except| that shows |space|,
// the most recently active first, or null.
BrowserWindowInterface* FindWindowShowing(Profile* profile,
                                          const SpaceModel& space,
                                          BrowserWindowInterface* except) {
  BrowserWindowInterface* found = nullptr;
  ProfileBrowserCollection::GetForProfile(profile)->ForEach(
      [&](BrowserWindowInterface* other) {
        if (other != except && IsUsableNormalWindow(other) &&
            &WindowFeatures::From(other)->space_model() == &space) {
          found = other;
        }
        return !found;
      },
      BrowserCollection::Order::kActivation);
  return found;
}

// Adds |node|'s live tabs, and those of its subtree, to |tabs|.
void CollectLiveTabs(const LiveTabRegistry& registry,
                     const SidebarNode* node,
                     std::vector<tabs::TabHandle>& tabs) {
  if (tabs::TabInterface* tab = registry.GetTab(node)) {
    tabs.push_back(tab->GetHandle());
  }
  for (const auto& child : node->children()) {
    CollectLiveTabs(registry, child.get(), tabs);
  }
}

// Whether |entry| shows in one of |profile|'s windows, or one of its tabs
// plays sound.
bool IsEntryInUse(Profile* profile,
                  const LiveTabRegistry& registry,
                  const SidebarNode& entry) {
  bool shown = false;
  ProfileBrowserCollection::GetForProfile(profile)->ForEach(
      [&](BrowserWindowInterface* browser) {
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features && !features->is_parking_window() &&
            !features->is_switching()) {
          // A placeholder shows its node too.
          const SidebarNode* node =
              features->tab_strip_bridge().GetShownNode(
                  browser->GetActiveTabInterface());
          shown = node && entry.Contains(node);
        }
        return !shown;
      },
      BrowserCollection::Order::kActivation);
  if (shown) {
    return true;
  }
  std::vector<tabs::TabHandle> tabs;
  CollectLiveTabs(registry, &entry, tabs);
  return std::ranges::any_of(tabs, [](const tabs::TabHandle& handle) {
    tabs::TabInterface* tab = handle.Get();
    return tab && tab->GetContents()->IsCurrentlyAudible();
  });
}

SwapWindowFactory& GetSwapWindowFactory() {
  static base::NoDestructor<SwapWindowFactory> factory;
  return *factory;
}

bool g_creating_swap_window = false;

BrowserWindowInterface* CreateSwapWindow(
    Profile* profile,
    const gfx::Rect& bounds,
    ui::mojom::WindowShowState show_state,
    BrowserWindowInterface* from) {
  base::AutoReset<bool> creating(&g_creating_swap_window, true);
  if (const SwapWindowFactory& factory = GetSwapWindowFactory()) {
    return factory.Run(profile, bounds, show_state);
  }
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL,
                                   profile, /*from_user_gesture=*/true);
  params.initial_bounds = bounds;
  params.initial_show_state = show_state;
  params.should_trigger_session_restore = false;
  // The sidebar starts as wide as the old window's, collapsed or not. Set
  // after the window is built (CopySidebarState()), the width never reached
  // the sidebar view, which keeps its own copy: the sidebar jumped to the
  // other profile's width on every switch.
  if (const tabs::VerticalTabStripStateController* sidebar =
          tabs::VerticalTabStripStateController::From(from)) {
    params.vertical_tab_strip_uncollapsed_width =
        sidebar->GetUncollapsedWidth();
    params.vertical_tab_strip_collapsed = sidebar->IsCollapsed();
  }
  return CreateBrowserWindow(std::move(params));
}

SwapPresenter& GetSwapPresenter() {
  static base::NoDestructor<SwapPresenter> presenter;
  return *presenter;
}

// The old window's sidebar, as the new one should start: same width, and
// collapsed or not.
void CopySidebarState(BrowserWindowInterface* from, BrowserWindowInterface* to) {
  tabs::VerticalTabStripStateController* old_state =
      tabs::VerticalTabStripStateController::From(from);
  tabs::VerticalTabStripStateController* new_state =
      tabs::VerticalTabStripStateController::From(to);
  if (!old_state || !new_state) {
    return;
  }
  new_state->SetUncollapsedWidth(old_state->GetUncollapsedWidth());
  if (new_state->IsCollapsed() != old_state->IsCollapsed()) {
    new_state->RequestCollapse(old_state->IsCollapsed());
  }
}

// The spaces |profile|'s windows can switch to, in the switcher's order.
std::vector<SpaceDirectory::Entry> GetSpacesOf(Profile* profile) {
  SpaceDirectory* directory = SpaceDirectory::GetIfExists();
  if (directory && directory->IsListed(profile)) {
    return directory->GetSpaces();
  }
  std::vector<SpaceDirectory::Entry> spaces;
  for (const auto& space : SpaceService::Get(profile)->space_list().spaces()) {
    spaces.push_back({profile, space.get()});
  }
  return spaces;
}

// Closes the parked tabs of |space|'s entries.
void CloseParkedTabs(SpaceService& service, const SpaceModel& space) {
  BrowserWindowInterface* parking = service.parking_window();
  if (!parking) {
    return;
  }
  TabStripModel* strip = parking->GetTabStripModel();
  for (int i = strip->count() - 1; i >= 0; --i) {
    tabs::TabInterface* tab = strip->GetTabAtIndex(i);
    SidebarNode* node = service.registry().GetNode(tab);
    if (node && space.Contains(node)) {
      service.registry().Unregister(tab);
      strip->CloseWebContentsAt(i, CLOSE_NONE);
    }
  }
}

// Removes |space|, which no window shows, from |profile|. Its last space
// can't go from its list, so the profile leaves the switcher instead, with a
// fresh space in place of |space| for when a window of it opens again.
void RemoveSpaceFromProfile(Profile* profile, SpaceModel& space) {
  SpaceService* service = SpaceService::Get(profile);
  SpaceList& list = service->space_list();
  if (list.spaces().size() < 2) {
    if (SpaceDirectory* directory = SpaceDirectory::GetIfExists()) {
      directory->Unlist(profile);
    }
    list.AddSpace(list.spaces().size(), u"Home");
    // Nothing can show what's left in its parking window: favorites' tabs.
    service->CloseParkingWindow();
  }
  list.RemoveSpace(space);
}

// Moves every window of |profile| showing |space|, closing or not, so no
// bridge outlives it, to |to|: rebinding, or swapping to another profile's
// space. A closing window can't swap: it rebinds to another space of its
// profile if there is one, else just empties. See EnterSpace() for
// |reopen_last_entry|.
void MoveWindowsOff(Profile* profile,
                    SpaceModel& space,
                    SpaceDirectory::Entry to,
                    bool reopen_last_entry = false) {
  std::vector<BrowserWindowInterface*> showing;
  ProfileBrowserCollection::GetForProfile(profile)->ForEach(
      [&](BrowserWindowInterface* browser) {
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features && !features->is_parking_window() &&
            !features->swapped_away() && &features->space_model() == &space) {
          showing.push_back(browser);
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  for (BrowserWindowInterface* browser : showing) {
    WindowFeatures* features = WindowFeatures::From(browser);
    if (to.profile == profile) {
      features->SwitchToSpace(*to.space);
    } else if (!browser->capabilities()->IsAttemptingToCloseBrowser()) {
      SwitchWindowToSpace(browser, to.profile, *to.space, reopen_last_entry);
    } else {
      SpaceModel* other = nullptr;
      for (const auto& own : SpaceService::Get(profile)->space_list().spaces()) {
        if (own.get() != &space) {
          other = own.get();
        }
      }
      if (other) {
        features->SwitchToSpace(*other);
      } else {
        features->LeaveForSwap();
      }
    }
  }
}

}  // namespace

namespace {
bool g_creating_window_outside_sidebar = false;
}  // namespace

base::AutoReset<bool> CreatingWindowOutsideSidebar() {
  return base::AutoReset<bool>(&g_creating_window_outside_sidebar, true);
}

DEFINE_USER_DATA(WindowFeatures);

WindowFeatures::WindowFeatures(BrowserWindowInterface* browser)
    : browser_(browser),
      service_(SpaceService::Get(browser->GetProfile())),
      space_(&service_->space_list().active_space()),
      is_parking_window_(service_->creating_parking_window()),
      outside_sidebar_(g_creating_window_outside_sidebar),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  service_->space_list().AddObserver(this);
  if (is_parking_window_) {
    return;
  }
  // A window of a profile lists its spaces in the switcher.
  if (browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
      SpaceDirectory::IsListable(browser->GetProfile())) {
    if (SpaceDirectory* directory = SpaceDirectory::Get()) {
      directory->List(browser->GetProfile());
    }
  }
  bridge_ = std::make_unique<TabStripBridge>(
      browser_, space_, &service_->space_list().favorites(), &registry(),
      &service_->restored_tab_matcher(*space_), !outside_sidebar_);
  active_tab_subscription_ =
      browser_->RegisterActiveTabDidChange(base::BindRepeating(
          &WindowFeatures::OnActiveTabChanged, base::Unretained(this)));
  did_become_active_subscription_ =
      browser_->RegisterDidBecomeActive(base::BindRepeating(
          &WindowFeatures::OnWindowBecameActive, base::Unretained(this)));
  if (!outside_sidebar_) {
    MaybeShowFirstSpaceAtStartup(browser_);
  }
}

WindowFeatures::~WindowFeatures() {
  service_->space_list().RemoveObserver(this);
  if (is_parking_window_) {
    service_->OnParkingWindowDestroyed(browser_);
    return;
  }
  // Quitting closes every window but may never destroy the profile, so don't
  // leave pending changes to the store's destructor.
  if (SidebarStore* store = service_->store()) {
    store->Flush();
  }
  // With the last window that could show the parked tabs gone, close them:
  // the profile's own last window, or for a listed profile, the last window
  // of every listed profile (any of them can switch to its spaces).
  Profile* profile = browser_->GetProfile();
  const bool listed = SpaceDirectory::IsListable(profile);
  bool other_window = false;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* other) {
        other_window =
            other != browser_ && IsUsableNormalWindow(other) &&
            (other->GetProfile() == profile ||
             (listed && SpaceDirectory::IsListable(other->GetProfile())));
        return !other_window;
      },
      BrowserCollection::Order::kActivation);
  if (other_window) {
    return;
  }
  base::flat_set<SpaceService*> services = {service_.get()};
  if (SpaceDirectory* directory = listed ? SpaceDirectory::GetIfExists()
                                         : nullptr) {
    for (const SpaceDirectory::Entry& entry : directory->GetSpaces()) {
      services.insert(SpaceService::Get(entry.profile));
    }
  }
  for (SpaceService* service : services) {
    if (service->parking_window()) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&SpaceService::CloseParkingWindow,
                                    service->GetWeakPtr()));
    }
  }
}

// static
WindowFeatures* WindowFeatures::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

void WindowFeatures::SwitchToSpace(SpaceModel& target) {
  CHECK(service_->space_list().FindSpace(target.id()) == &target);
  if (is_parking_window_ || &target == space_) {
    return;
  }
  base::AutoReset<bool> switching(&switching_, true);
  TabStripModel* strip = browser_->GetTabStripModel();
  SpaceModel& leaving_space = *space_;

  // 1. The strip's tabs belong to the space being left, except favorites'.
  std::vector<tabs::TabHandle> leaving;
  for (int i = 0; i < strip->count(); ++i) {
    tabs::TabInterface* tab = strip->GetTabAtIndex(i);
    SidebarNode* node = registry().GetNode(tab);
    if (!node || !service_->space_list().favorites().Contains(node)) {
      leaving.push_back(tab->GetHandle());
    }
  }

  // 2.
  bridge_.reset();
  space_ = &target;

  // 3. to 5.
  EnterCurrentSpace();

  // 6. The old space's tabs move out. The window no longer shows any of them,
  // so the bridge puts up no placeholders.
  MoveTabsOut(leaving, leaving_space, /*keep_one=*/true);

  service_->space_list().SetActiveSpace(target);
  switching_ = false;
  preview_space_id_ = base::Uuid();
  preview_progress_ = 0;
  for (Observer& observer : observers_) {
    observer.OnSpaceSwitched();
  }
}

void WindowFeatures::EnterSpace(bool reopen_last_entry) {
  CHECK(!is_parking_window_);
  base::AutoReset<bool> switching(&switching_, true);
  bridge_.reset();
  EnterCurrentSpace(reopen_last_entry);
  switching_ = false;
  for (Observer& observer : observers_) {
    observer.OnSpaceSwitched();
  }
}

void WindowFeatures::BeginSwap() {
  CHECK(!is_parking_window_);
  swapped_away_ = true;
  for (Observer& observer : observers_) {
    observer.OnSwappedAway();
  }
}

void WindowFeatures::LeaveForSwap() {
  CHECK(swapped_away_);
  bridge_.reset();
  TabStripModel* strip = browser_->GetTabStripModel();
  std::vector<tabs::TabHandle> leaving;
  for (int i = 0; i < strip->count(); ++i) {
    leaving.push_back(strip->GetTabAtIndex(i)->GetHandle());
  }
  MoveTabsOut(leaving, *space_, /*keep_one=*/false);
}

void WindowFeatures::EnterCurrentSpace(bool reopen_last_entry) {
  SpaceModel& target = *space_;
  TabStripModel* strip = browser_->GetTabStripModel();

  // 3. The target space's parked tabs come back, in the background.
  std::vector<tabs::TabInterface*> arrived;
  if (BrowserWindowInterface* parking = service_->parking_window()) {
    TabStripModel* parked = parking->GetTabStripModel();
    for (int i = 0; i < parked->count();) {
      SidebarNode* node = registry().GetNode(parked->GetTabAtIndex(i));
      if (!node || !target.Contains(node)) {
        ++i;
        continue;
      }
      std::unique_ptr<tabs::TabModel> tab = parked->DetachTabAtForInsertion(i);
      arrived.push_back(tab.get());
      strip->InsertDetachedTabAt(strip->count(), std::move(tab),
                                 AddTabTypes::ADD_NONE);
    }
  }

  // 4. Every tab already has a node, so the bridge adds none.
  bridge_ = std::make_unique<TabStripBridge>(
      browser_, space_, &service_->space_list().favorites(), &registry(),
      &service_->restored_tab_matcher(target), !outside_sidebar_);

  // 5. Show what the space showed last, loading it if it isn't open (after a
  // restart, the tabs of spaces no window showed come back unloaded), else
  // another of its open tabs. A space with none, and no entry it showed,
  // shows the empty tab, whose command bar asks what to open.
  SidebarNode* last = service_->GetLastActiveNode(target);
  tabs::TabInterface* shown = last ? bridge_->GetTab(last) : nullptr;
  // Open in no window: a tab of it still in a window swapping away comes
  // over when that window leaves (OnSpaceTabsArrived()).
  if (!shown && last && !registry().GetTab(last)) {
    shown = bridge_->OpenNode(last);
  }
  if (!shown && !arrived.empty()) {
    shown = arrived.front();
  }
  if (!shown && reopen_last_entry) {
    // The entry it showed, as far as is known.
    SidebarNode* entry = last ? last : FindFirstTab(target.pinned_root());
    if (!entry) {
      entry = FindFirstTab(target.today_root());
    }
    if (entry) {
      shown = bridge_->OpenNode(entry);
    }
  }
  entered_empty_tab_ = tabs::TabHandle();
  if (!shown) {
    // Its tabs may still be in a window swapping away from it, which hands
    // them over when it leaves (OnSpaceTabsArrived()).
    OpenEmptyTab(browser_);
    if (tabs::TabInterface* empty = browser_->GetActiveTabInterface()) {
      entered_empty_tab_ = empty->GetHandle();
    }
  }
  if (shown && strip->GetIndexOfTab(shown) != TabStripModel::kNoTab) {
    strip->ActivateTabAt(strip->GetIndexOfTab(shown));
    // Already active if it came into an empty strip, which activated it
    // before the bridge was there to note it (OnActiveTabChanged()).
    if (SidebarNode* node = bridge_->GetShownNode(shown);
        node && target.Contains(node)) {
      service_->SetLastActiveNode(target, *node);
    }
  }
}

void WindowFeatures::MoveTabsOut(const std::vector<tabs::TabHandle>& leaving,
                                 SpaceModel& leaving_space,
                                 bool keep_one) {
  TabStripModel* strip = browser_->GetTabStripModel();
  BrowserWindowInterface* destination = FindOtherWindowShowing(leaving_space);
  std::vector<tabs::TabInterface*> handed_over;
  for (const tabs::TabHandle& handle : leaving) {
    tabs::TabInterface* tab = handle.Get();
    const int index = tab ? strip->GetIndexOfTab(tab) : TabStripModel::kNoTab;
    if (index == TabStripModel::kNoTab || (keep_one && strip->count() == 1)) {
      continue;
    }
    SidebarNode* node = registry().GetNode(tab);
    // Placeholders, and blank Today tabs, which aren't worth keeping. The
    // node's URL can lag a navigation by Browser's coalesced UI update, so
    // the tab's own URL counts too.
    if (!node || (leaving_space.today_root()->Contains(node) &&
                  !IsRestorableURL(node->url()) &&
                  !IsRestorableURL(tab->GetContents()->GetVisibleURL()))) {
      if (node) {
        registry().Unregister(tab);
        leaving_space.Remove(node);
      }
      strip->CloseWebContentsAt(index, CLOSE_NONE);
      continue;
    }
    // Favorites' tabs (only on a swap) go to the parking window: every
    // window of the profile shows them, so none is theirs.
    BrowserWindowInterface* to =
        leaving_space.Contains(node) ? destination : nullptr;
    if (!to) {
      to = service_->GetOrCreateParkingWindow();
    }
    TabStripModel* destination_strip = to->GetTabStripModel();
    std::unique_ptr<tabs::TabModel> detached =
        strip->DetachTabAtForInsertion(index);
    if (to == destination) {
      handed_over.push_back(detached.get());
    }
    destination_strip->InsertDetachedTabAt(destination_strip->count(),
                                           std::move(detached),
                                           AddTabTypes::ADD_NONE);
  }
  if (!handed_over.empty()) {
    if (WindowFeatures* features = WindowFeatures::From(destination)) {
      features->OnSpaceTabsArrived(handed_over);
    }
  }
}

void WindowFeatures::OnSpaceTabsArrived(
    const std::vector<tabs::TabInterface*>& arrived) {
  tabs::TabInterface* empty = entered_empty_tab_.Get();
  entered_empty_tab_ = tabs::TabHandle();
  // The person may have moved on since: another tab, or a page typed into
  // the empty tab's command bar.
  if (!bridge_ || !empty || empty != browser_->GetActiveTabInterface() ||
      !IsEmptyTab(empty)) {
    return;
  }
  SidebarNode* last = service_->GetLastActiveNode(*space_);
  tabs::TabInterface* shown = last ? bridge_->GetTab(last) : nullptr;
  if (!shown || !std::ranges::contains(arrived, shown)) {
    shown = arrived.front();
  }
  // The empty tab closes once another tab shows (empty_tab.h).
  TabStripModel* strip = browser_->GetTabStripModel();
  const int index = strip->GetIndexOfTab(shown);
  if (index != TabStripModel::kNoTab) {
    strip->ActivateTabAt(index);
  }
}

void WindowFeatures::SetSpacePreview(SpaceModel* space, double progress) {
  if (space == space_) {
    space = nullptr;
  }
  preview_space_id_ = space ? space->id() : base::Uuid();
  preview_progress_ = space ? std::clamp(progress, 0.0, 1.0) : 0;
  for (Observer& observer : observers_) {
    observer.OnSpacePreviewChanged();
  }
}

SpaceModel* WindowFeatures::preview_space() {
  if (!preview_space_id_.is_valid()) {
    return nullptr;
  }
  if (SpaceModel* space = service_->space_list().FindSpace(preview_space_id_)) {
    return space;
  }
  // Another profile's.
  SpaceDirectory* directory = SpaceDirectory::GetIfExists();
  return directory ? directory->FindSpace(preview_space_id_).space.get()
                   : nullptr;
}

void WindowFeatures::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void WindowFeatures::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void WindowFeatures::OnSpaceWillBeRemoved(SpaceModel& space) {
  // Windows showing a space switch away before it goes, except one that
  // swapped away and is closing: it takes the space the list falls back to.
  if (&space == space_) {
    CHECK(swapped_away_ || is_parking_window_);
    // A bridge on it would outlive it. The window shows its tabs until the
    // swap ends; nothing mirrors them any more.
    bridge_.reset();
    space_ = &service_->space_list().active_space();
  }
}

void WindowFeatures::OnActiveTabChanged(BrowserWindowInterface* browser) {
  if (!bridge_) {
    // Mid-switch.
    return;
  }
  const SidebarNode* node =
      bridge_->GetShownNode(browser_->GetActiveTabInterface());
  if (node && space_->Contains(node)) {
    service_->SetLastActiveNode(*space_, *node);
  }
}

void WindowFeatures::OnWindowBecameActive(BrowserWindowInterface* browser) {
  // New windows open in the space used last.
  service_->space_list().SetActiveSpace(*space_);
}

BrowserWindowInterface* WindowFeatures::FindOtherWindowShowing(
    const SpaceModel& space) {
  return FindWindowShowing(browser_->GetProfile(), space, browser_);
}

SidebarNode* PredictShownEntry(SpaceService& service, SpaceModel& space) {
  LiveTabRegistry& registry = service.registry();
  // Shown even if it isn't open: the switch loads it.
  if (SidebarNode* last = service.GetLastActiveNode(space)) {
    return last;
  }
  for (SidebarNode* root : {space.pinned_root(), space.today_root()}) {
    if (SidebarNode* tab = FindFirstLiveTab(root, registry)) {
      return tab;
    }
  }
  // Nothing open, and nothing it showed: the empty tab, which has no entry.
  return nullptr;
}

void MoveEntryToSpace(Profile* profile, SidebarNode* node, SpaceModel& target) {
  SpaceService* service = SpaceService::Get(profile);
  SpaceModel* source = service->space_list().FindSpaceOf(node);
  if (!source || service->space_list().FindSpace(target.id()) != &target) {
    return;
  }
  SidebarNode* new_parent = source->pinned_root()->Contains(node)
                                ? target.pinned_root()
                                : target.today_root();
  MoveEntry(profile, node, target, new_parent, new_parent->children().size());
}

void MoveEntry(Profile* profile,
               SidebarNode* node,
               SpaceModel& target,
               SidebarNode* new_parent,
               size_t index) {
  SpaceService* service = SpaceService::Get(profile);
  SpaceList& list = service->space_list();
  SpaceModel* source = list.favorites().Contains(node)
                           ? &list.favorites()
                           : list.FindSpaceOf(node);
  if (!source || (&target != &list.favorites() &&
                  list.FindSpace(target.id()) != &target) ||
      !source->CanMoveToSpace(node, target, new_parent)) {
    return;
  }
  // Favorites show in every window, so their tabs stay where they are.
  if (&target == &list.favorites()) {
    source->MoveToSpace(node, target, new_parent, index);
    return;
  }
  LiveTabRegistry& registry = service->registry();
  std::vector<tabs::TabHandle> moving;
  CollectLiveTabs(registry, node, moving);

  // The tabs that leave a window mustn't be the one it shows: a placeholder
  // would take its place. Show another tab, or a new one.
  auto is_moving = [&](const tabs::TabInterface* tab) {
    return std::ranges::any_of(moving, [&](const tabs::TabHandle& handle) {
      return handle.Get() == tab;
    });
  };
  for (const tabs::TabHandle& handle : moving) {
    tabs::TabInterface* tab = handle.Get();
    BrowserWindowInterface* window = tab->GetBrowserWindowInterface();
    WindowFeatures* features = window ? WindowFeatures::From(window) : nullptr;
    if (!features || features->is_parking_window() ||
        window->GetActiveTabInterface() != tab) {
      continue;
    }
    // The nearest tab before it that stays, else the first after it.
    TabStripModel* strip = window->GetTabStripModel();
    const int active = strip->GetIndexOfTab(tab);
    int staying = TabStripModel::kNoTab;
    for (int i = 0; i < strip->count(); ++i) {
      if (is_moving(strip->GetTabAtIndex(i))) {
        continue;
      }
      if (i > active) {
        if (staying == TabStripModel::kNoTab) {
          staying = i;
        }
        break;
      }
      staying = i;
    }
    if (staying == TabStripModel::kNoTab) {
      OpenEmptyTab(window);
    } else {
      strip->ActivateTabAt(staying);
    }
  }

  source->MoveToSpace(node, target, new_parent, index);

  BrowserWindowInterface* destination =
      FindWindowShowing(profile, target, nullptr);
  for (const tabs::TabHandle& handle : moving) {
    tabs::TabInterface* tab = handle.Get();
    BrowserWindowInterface* window = tab ? tab->GetBrowserWindowInterface()
                                         : nullptr;
    WindowFeatures* features = window ? WindowFeatures::From(window) : nullptr;
    // Parked tabs stay parked: the parking window holds every space's.
    if (!features || features->is_parking_window() ||
        &features->space_model() == &target) {
      continue;
    }
    if (!destination) {
      destination = service->GetOrCreateParkingWindow();
    }
    TabStripModel* strip = window->GetTabStripModel();
    TabStripModel* destination_strip = destination->GetTabStripModel();
    destination_strip->InsertDetachedTabAt(
        destination_strip->count(),
        strip->DetachTabAtForInsertion(strip->GetIndexOfTab(tab)),
        AddTabTypes::ADD_NONE);
  }
}

void DeleteSpace(Profile* profile, SpaceModel& space) {
  SpaceService* service = SpaceService::Get(profile);
  SpaceList& list = service->space_list();
  if (list.FindSpace(space.id()) != &space) {
    return;
  }
  // Windows showing it go to its neighbor in the switcher, which can be
  // another profile's. Not the last space of all.
  std::vector<SpaceDirectory::Entry> spaces = GetSpacesOf(profile);
  auto it = std::ranges::find(spaces, &space, &SpaceDirectory::Entry::space);
  if (it == spaces.end() || spaces.size() < 2) {
    return;
  }
  const SpaceDirectory::Entry fallback = it + 1 != spaces.end() ? *(it + 1)
                                                                : *(it - 1);
  MoveWindowsOff(profile, space, fallback);

  // Its live tabs are all parked now.
  CloseParkedTabs(*service, space);
  RemoveSpaceFromProfile(profile, space);
}

void MoveSpaceToProfile(Profile* from, SpaceModel& space, Profile* to) {
  SpaceService* source = SpaceService::Get(from);
  SpaceDirectory* directory = SpaceDirectory::Get();
  if (from == to || !directory || !SpaceDirectory::IsListable(from) ||
      !SpaceDirectory::IsListable(to) ||
      source->space_list().FindSpace(space.id()) != &space) {
    return;
  }
  const bool was_listed = directory->IsListed(to);
  directory->List(to);
  SpaceService* target = SpaceService::Get(to);

  // 1. Its copy in |to|, where it was in the switcher.
  SpaceList& target_list = target->space_list();
  // It keeps its ID, and so its place in the switcher: sync sees the same
  // space leave one profile's account and come into the other's
  // (docs/design/sync.md, "Slice 5 notes").
  const bool same_id = !target_list.FindSpace(space.id());
  SpaceModel& moved = *target_list.AddSpace(
      target_list.spaces().size(), space.name(),
      same_id ? std::optional<base::Uuid>(space.id()) : std::nullopt);
  moved.SetColor(space.color());
  moved.SetGradient(space.gradient());
  moved.SetIntensity(space.intensity());
  moved.SetGrain(space.grain());
  moved.SetAppearance(space.appearance());
  moved.SetIcon(space.icon());
  moved.SetArchiveAfter(space.archive_after());
  for (const ArchivedEntry& entry : space.archive()) {
    moved.AddArchived(entry);
  }
  if (!same_id) {
    directory->PlaceAfter(moved, space);
  }

  // 2. Live tabs can't change profile: they close, and their entries stay,
  // unloaded. Unregistered first, so closing leaves the entries.
  LiveTabRegistry& registry = source->registry();
  std::vector<tabs::TabHandle> tabs;
  CollectLiveTabs(registry, space.pinned_root(), tabs);
  CollectLiveTabs(registry, space.today_root(), tabs);
  for (const tabs::TabHandle& handle : tabs) {
    tabs::TabInterface* tab = handle.Get();
    // Their entries keep the page they show, which the new window loads: a
    // navigation reaches the entry up to 200 ms late (Browser's coalesced UI
    // update), and the tab won't be there to bring it.
    BrowserWindowInterface* window = tab->GetBrowserWindowInterface();
    WindowFeatures* features = window ? WindowFeatures::From(window) : nullptr;
    if (features && features->has_tab_strip_bridge()) {
      features->tab_strip_bridge().UpdateNode(tab);
    }
    registry.Unregister(tab);
  }

  // 3. The entries move: the same nodes, as between spaces of a profile.
  SidebarNode* last = source->GetLastActiveNode(space);
  for (auto [from_root, to_root] :
       {std::pair(space.pinned_root(), moved.pinned_root()),
        std::pair(space.today_root(), moved.today_root())}) {
    while (!from_root->children().empty()) {
      space.MoveToSpace(from_root->children().front().get(), moved, to_root,
                        to_root->children().size());
    }
  }
  if (last) {
    target->SetLastActiveNode(moved, *last);
  }
  // A profile new to the switcher comes with this space only, not with the
  // empty one it starts with (a new profile's "Home"). No window shows those:
  // a window lists its profile.
  if (!was_listed) {
    std::vector<SpaceModel*> empty;
    for (const auto& other : target_list.spaces()) {
      if (other.get() != &moved && other->pinned_root()->children().empty() &&
          other->today_root()->children().empty() &&
          other->archive().empty()) {
        empty.push_back(other.get());
      }
    }
    for (SpaceModel* other : empty) {
      target_list.RemoveSpace(*other);
    }
  }

  // 4. Windows showing it swap to it: the new window loads the entry it
  // showed. The old one's tabs, now without entries, close.
  MoveWindowsOff(from, space, {to, &moved}, /*reopen_last_entry=*/true);

  // 5. The rest of its tabs, parked or elsewhere. Those of a window swapping
  // away stay until the new window shows, then close with it.
  for (const tabs::TabHandle& handle : tabs) {
    tabs::TabInterface* tab = handle.Get();
    BrowserWindowInterface* window =
        tab ? tab->GetBrowserWindowInterface() : nullptr;
    WindowFeatures* features = window ? WindowFeatures::From(window) : nullptr;
    if (window && !(features && features->swapped_away())) {
      TabStripModel* strip = window->GetTabStripModel();
      strip->CloseWebContentsAt(strip->GetIndexOfTab(tab), CLOSE_NONE);
    }
  }
  RemoveSpaceFromProfile(from, space);
}

void AssignSpaceToProfile(const base::Uuid& space_id,
                          std::optional<base::FilePath> profile_path) {
  // Then once its sidebar file is in, so its spaces are known.
  auto assign = base::BindOnce(
      [](const base::Uuid& space_id, Profile* to) {
        if (!to) {
          return;
        }
        SpaceService::Get(to)->RunWhenLoaded(base::BindOnce(
            [](const base::Uuid& space_id, base::WeakPtr<Profile> to) {
              SpaceDirectory* directory = SpaceDirectory::GetIfExists();
              SpaceDirectory::Entry entry =
                  directory ? directory->FindSpace(space_id)
                            : SpaceDirectory::Entry();
              if (to && entry.space) {
                MoveSpaceToProfile(entry.profile, *entry.space, to.get());
              }
            },
            space_id, to->GetWeakPtr()));
      },
      space_id);
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_path) {
    ProfileAttributesStorage& storage =
        profile_manager->GetProfileAttributesStorage();
    ProfileManager::CreateMultiProfileAsync(
        storage.ChooseNameForNewProfile(),
        storage.ChooseAvatarIconIndexForNewProfile(), /*is_hidden=*/false,
        std::move(assign));
    return;
  }
  // Runs |assign| with null if it fails.
  profile_manager->LoadProfileByPath(*profile_path, /*incognito=*/false,
                                     std::move(assign));
}

BrowserWindowInterface* SwitchWindowToSpace(BrowserWindowInterface* browser,
                                            Profile* profile,
                                            SpaceModel& space,
                                            bool reopen_last_entry) {
  WindowFeatures* features = WindowFeatures::From(browser);
  if (!features || features->is_parking_window() ||
      features->swapped_away() ||
      SpaceService::Get(profile)->space_list().FindSpace(space.id()) !=
          &space) {
    return nullptr;
  }
  if (profile == browser->GetProfile()) {
    features->SwitchToSpace(space);
    return browser;
  }
  if (!SpaceDirectory::IsListable(profile) ||
      !SpaceDirectory::IsListable(browser->GetProfile())) {
    return nullptr;
  }
  // 1. The profile's next window opens on |space|.
  SpaceService::Get(profile)->space_list().SetActiveSpace(space);
  // The old window takes no more requests, and no second swap.
  features->BeginSwap();
  // 2.
  ui::BaseWindow* old_window = browser->GetWindow();
  const ui::mojom::WindowShowState show_state =
      old_window->IsFullscreen()  ? ui::mojom::WindowShowState::kFullscreen
      : old_window->IsMaximized() ? ui::mojom::WindowShowState::kMaximized
                                  : ui::mojom::WindowShowState::kDefault;
  BrowserWindowInterface* window =
      CreateSwapWindow(profile, old_window->GetRestoredBounds(), show_state,
                       browser);
  WindowFeatures* new_features = WindowFeatures::From(window);
  CHECK(&new_features->space_model() == &space);
  CopySidebarState(browser, window);
  // 3.
  new_features->EnterSpace(reopen_last_entry);
  // 4. and 5. It takes the old window's place once it has painted; then the
  // old window empties, and closes on its own.
  base::OnceClosure leave = base::BindOnce(
      [](base::WeakPtr<BrowserWindowInterface> old) {
        WindowFeatures* old_features =
            old ? WindowFeatures::From(old.get()) : nullptr;
        if (old_features) {
          old_features->LeaveForSwap();
        }
      },
      browser->GetWeakPtr());
  if (const SwapPresenter& presenter = GetSwapPresenter()) {
    presenter.Run(browser, window, std::move(leave));
  } else {
    window->GetWindow()->Show();
    std::move(leave).Run();
  }
  return window;
}

bool IsCreatingSwapWindow() {
  return g_creating_swap_window;
}

void SetSwapPresenter(SwapPresenter presenter) {
  GetSwapPresenter() = std::move(presenter);
}

std::vector<SpaceDirectory::Entry> GetSwitchableSpaces(
    BrowserWindowInterface* browser) {
  Profile* profile = browser->GetProfile();
  SpaceDirectory* directory =
      SpaceDirectory::IsListable(profile) ? SpaceDirectory::Get() : nullptr;
  if (directory && directory->IsListed(profile)) {
    return directory->GetSpaces();
  }
  std::vector<SpaceDirectory::Entry> spaces;
  for (const auto& space : SpaceService::Get(profile)->space_list().spaces()) {
    spaces.push_back({profile, space.get()});
  }
  return spaces;
}

SpaceDirectory::Entry FindSwitchableSpace(BrowserWindowInterface* browser,
                                          const base::Uuid& id) {
  for (const SpaceDirectory::Entry& entry : GetSwitchableSpaces(browser)) {
    if (entry.space->id() == id) {
      return entry;
    }
  }
  return {};
}

void SetSwapWindowFactoryForTesting(SwapWindowFactory factory) {
  GetSwapWindowFactory() = std::move(factory);
}

bool HandOffTabsBeforeClose(BrowserWindowInterface* browser) {
  if (browser_shutdown::IsTryingToQuit() ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      browser->GetTabStripModel()->empty()) {
    return false;
  }
  WindowFeatures* source = WindowFeatures::From(browser);
  if (!source || source->is_parking_window()) {
    return false;
  }
  // The window the user was in last, among those showing the same space:
  // where they'd look for the tabs.
  WindowFeatures* target = nullptr;
  ProfileBrowserCollection::GetForProfile(browser->GetProfile())
      ->ForEach(
          [&](BrowserWindowInterface* other) {
            if (other != browser && IsUsableNormalWindow(other) &&
                &WindowFeatures::From(other)->space_model() ==
                    &source->space_model()) {
              target = WindowFeatures::From(other);
            }
            return !target;
          },
          BrowserCollection::Order::kActivation);
  if (!target) {
    return false;
  }
  source->tab_strip_bridge().HandOffTabsTo(target->tab_strip_bridge());
  return true;
}

bool KeepWindowWithNoTabs(BrowserWindowInterface* browser) {
  if (browser_shutdown::IsTryingToQuit() ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !browser->GetTabStripModel()->empty()) {
    return false;
  }
  WindowFeatures* features = WindowFeatures::From(browser);
  if (!features || features->is_parking_window() ||
      features->swapped_away() || !features->has_tab_strip_bridge()) {
    return false;
  }
  return features->tab_strip_bridge().KeepWindowWithNoTabs();
}

BrowserWindowInterface* FindLastActiveWindow(Profile* profile) {
  BrowserWindowInterface* found = nullptr;
  ProfileBrowserCollection::GetForProfile(profile)->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (IsUsableNormalWindow(browser)) {
          found = browser;
        }
        return !found;
      },
      BrowserCollection::Order::kActivation);
  return found;
}

BrowserWindowInterface* FindLastActiveWindowShowing(Profile* profile,
                                                    const SpaceModel& space) {
  return FindWindowShowing(profile, space, /*except=*/nullptr);
}

bool IsInParkingWindow(content::WebContents* contents) {
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  WindowFeatures* features = browser ? WindowFeatures::From(browser) : nullptr;
  return features && features->is_parking_window();
}

bool RevealParkedTab(content::WebContents* contents) {
  if (!IsInParkingWindow(contents)) {
    return false;
  }
  tabs::TabInterface* tab = tabs::TabInterface::GetFromContents(contents);
  Profile* profile = tab->GetBrowserWindowInterface()->GetProfile();
  SpaceService* service = SpaceService::Get(profile);
  SidebarNode* node = service->registry().GetNode(tab);
  if (!node) {
    // Never shows the parking window.
    return true;
  }
  // The window the user was in last, of any listed profile.
  BrowserWindowInterface* window = nullptr;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (IsUsableNormalWindow(browser) &&
            !browser->GetProfile()->IsOffTheRecord() &&
            (browser->GetProfile() == profile ||
             (SpaceDirectory::IsListable(profile) &&
              SpaceDirectory::IsListable(browser->GetProfile())))) {
          window = browser;
        }
        return !window;
      },
      BrowserCollection::Order::kActivation);
  if (!window) {
    return true;
  }
  // A favorite shows in every space of its profile.
  SpaceModel* space = service->space_list().FindSpaceOf(node);
  if (!space) {
    space = window->GetProfile() == profile
                ? &WindowFeatures::From(window)->space_model()
                : &service->space_list().active_space();
  }
  // Before a switch to another profile: the window that comes in takes the
  // native window, and must not show on its own.
  window->GetWindow()->Activate();
  if (window->GetProfile() != profile ||
      &WindowFeatures::From(window)->space_model() != space) {
    window = SwitchWindowToSpace(window, profile, *space);
    if (!window) {
      return true;
    }
  }
  if (tabs::TabInterface* shown =
          WindowFeatures::From(window)->tab_strip_bridge().OpenNode(node)) {
    TabStripModel* strip = window->GetTabStripModel();
    strip->ActivateTabAt(strip->GetIndexOfTab(shown));
  }
  return true;
}

bool IsEntryInUse(Profile* profile, const SidebarNode& entry) {
  return IsEntryInUse(profile, SpaceService::Get(profile)->registry(), entry);
}

bool IsEntryShownInFocusedWindow(Profile* profile, const SidebarNode& entry) {
  bool shown = false;
  ProfileBrowserCollection::GetForProfile(profile)->ForEach(
      [&](BrowserWindowInterface* browser) {
        WindowFeatures* features = WindowFeatures::From(browser);
        if (browser->IsActive() && features &&
            !features->is_parking_window() && !features->is_switching()) {
          const SidebarNode* node =
              features->tab_strip_bridge().GetShownNode(
                  browser->GetActiveTabInterface());
          shown = node && entry.Contains(node);
        }
        return !shown;
      },
      BrowserCollection::Order::kActivation);
  return shown;
}

void ArchiveIdleTodayEntries(Profile* profile, base::Time now) {
  // What a window shows is seen now: its time keeps up, so other devices,
  // whose archivers go by the synced time, don't archive it either
  // (docs/design/sync.md, "Slice 3b notes").
  ProfileBrowserCollection::GetForProfile(profile)->ForEach(
      [&](BrowserWindowInterface* browser) {
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features && !features->is_parking_window() &&
            !features->is_switching()) {
          SpaceModel& space = features->space_model();
          SidebarNode* node = features->tab_strip_bridge().GetShownNode(
              browser->GetActiveTabInterface());
          if (node && node->type() == SidebarNode::Type::kTab &&
              space.today_root()->Contains(node)) {
            space.SetLastActive(node, now);
          }
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  SpaceService* service = SpaceService::Get(profile);
  for (const auto& space : service->space_list().spaces()) {
    space->ArchiveIdleEntries(now, [&](const SidebarNode& entry) {
      return IsEntryInUse(profile, service->registry(), entry);
    });
  }
}

}  // namespace compass
