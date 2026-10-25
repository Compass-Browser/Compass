// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/window_features.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
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
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/sidebar/sidebar_serializer.h"
#include "strata/browser/window_sync/sidebar_store.h"
#include "strata/browser/window_sync/space_directory.h"
#include "ui/base/base_window.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

namespace strata {

namespace {

// The first tab entry of |space|, pinned first, depth first.
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

BrowserWindowInterface* CreateSwapWindow(Profile* profile,
                                         const gfx::Rect& bounds,
                                         bool maximized) {
  if (const SwapWindowFactory& factory = GetSwapWindowFactory()) {
    return factory.Run(profile, bounds, maximized);
  }
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL,
                                   profile, /*from_user_gesture=*/true);
  params.initial_bounds = bounds;
  params.initial_show_state = maximized ? ui::mojom::WindowShowState::kMaximized
                                        : ui::mojom::WindowShowState::kDefault;
  params.should_trigger_session_restore = false;
  return CreateBrowserWindow(std::move(params));
}

void OpenNewTab(BrowserWindowInterface* browser) {
  NavigateParams params(browser, GURL(chrome::kChromeUINewTabURL),
                        ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
}

}  // namespace

DEFINE_USER_DATA(WindowFeatures);

WindowFeatures::WindowFeatures(BrowserWindowInterface* browser)
    : browser_(browser),
      service_(SpaceService::Get(browser->GetProfile())),
      space_(&service_->space_list().active_space()),
      is_parking_window_(service_->creating_parking_window()),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  if (is_parking_window_) {
    return;
  }
  bridge_ = std::make_unique<TabStripBridge>(
      browser_, space_, &service_->space_list().favorites(), &registry(),
      &service_->restored_tab_matcher(*space_));
  active_tab_subscription_ =
      browser_->RegisterActiveTabDidChange(base::BindRepeating(
          &WindowFeatures::OnActiveTabChanged, base::Unretained(this)));
  did_become_active_subscription_ =
      browser_->RegisterDidBecomeActive(base::BindRepeating(
          &WindowFeatures::OnWindowBecameActive, base::Unretained(this)));
}

WindowFeatures::~WindowFeatures() {
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
  const bool listed = SpaceDirectory::IsListed(profile);
  bool other_window = false;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* other) {
        other_window =
            other != browser_ && IsUsableNormalWindow(other) &&
            (other->GetProfile() == profile ||
             (listed && SpaceDirectory::IsListed(other->GetProfile())));
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

void WindowFeatures::EnterSpace() {
  CHECK(!is_parking_window_);
  base::AutoReset<bool> switching(&switching_, true);
  bridge_.reset();
  EnterCurrentSpace();
  switching_ = false;
  for (Observer& observer : observers_) {
    observer.OnSpaceSwitched();
  }
}

void WindowFeatures::LeaveForSwap() {
  CHECK(!is_parking_window_);
  swapped_away_ = true;
  bridge_.reset();
  for (Observer& observer : observers_) {
    observer.OnSwappedAway();
  }
  TabStripModel* strip = browser_->GetTabStripModel();
  std::vector<tabs::TabHandle> leaving;
  for (int i = 0; i < strip->count(); ++i) {
    leaving.push_back(strip->GetTabAtIndex(i)->GetHandle());
  }
  MoveTabsOut(leaving, *space_, /*keep_one=*/false);
}

void WindowFeatures::EnterCurrentSpace() {
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
      &service_->restored_tab_matcher(target));

  // 5. Show what the space showed last.
  SidebarNode* last = service_->GetLastActiveNode(target);
  tabs::TabInterface* shown = last ? bridge_->GetTab(last) : nullptr;
  if (!shown && !arrived.empty()) {
    shown = arrived.front();
  }
  if (!shown) {
    SidebarNode* entry = last ? last : FindFirstTab(target.pinned_root());
    if (!entry) {
      entry = FindFirstTab(target.today_root());
    }
    if (entry) {
      shown = bridge_->OpenNode(entry);
    } else {
      OpenNewTab(browser_);
    }
  }
  if (shown && strip->GetIndexOfTab(shown) != TabStripModel::kNoTab) {
    strip->ActivateTabAt(strip->GetIndexOfTab(shown));
  }
}

void WindowFeatures::MoveTabsOut(const std::vector<tabs::TabHandle>& leaving,
                                 SpaceModel& leaving_space,
                                 bool keep_one) {
  TabStripModel* strip = browser_->GetTabStripModel();
  BrowserWindowInterface* destination = FindOtherWindowShowing(leaving_space);
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
    destination_strip->InsertDetachedTabAt(destination_strip->count(),
                                           strip->DetachTabAtForInsertion(index),
                                           AddTabTypes::ADD_NONE);
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
  SidebarNode* last = service.GetLastActiveNode(space);
  if (last && registry.GetTab(last)) {
    return last;
  }
  for (SidebarNode* root : {space.pinned_root(), space.today_root()}) {
    if (SidebarNode* tab = FindFirstLiveTab(root, registry)) {
      return tab;
    }
  }
  if (last) {
    return last;
  }
  SidebarNode* first = FindFirstTab(space.pinned_root());
  return first ? first : FindFirstTab(space.today_root());
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
      OpenNewTab(window);
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
  const auto& spaces = list.spaces();
  auto it = std::ranges::find(spaces, &space, &std::unique_ptr<SpaceModel>::get);
  if (it == spaces.end() || spaces.size() < 2) {
    return;
  }
  SpaceModel& fallback = it + 1 != spaces.end() ? **(it + 1) : **(it - 1);

  // Every window bound to it, closing or not, so no bridge outlives it.
  std::vector<WindowFeatures*> showing;
  ProfileBrowserCollection::GetForProfile(profile)->ForEach(
      [&](BrowserWindowInterface* browser) {
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features && !features->is_parking_window() &&
            &features->space_model() == &space) {
          showing.push_back(features);
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  for (WindowFeatures* features : showing) {
    features->SwitchToSpace(fallback);
  }

  // Its live tabs are all parked now.
  if (BrowserWindowInterface* parking = service->parking_window()) {
    TabStripModel* strip = parking->GetTabStripModel();
    for (int i = strip->count() - 1; i >= 0; --i) {
      tabs::TabInterface* tab = strip->GetTabAtIndex(i);
      SidebarNode* node = service->registry().GetNode(tab);
      if (node && space.Contains(node)) {
        service->registry().Unregister(tab);
        strip->CloseWebContentsAt(i, CLOSE_NONE);
      }
    }
  }
  list.RemoveSpace(space);
}

BrowserWindowInterface* SwitchWindowToSpace(BrowserWindowInterface* browser,
                                            Profile* profile,
                                            SpaceModel& space) {
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
  if (!SpaceDirectory::IsListed(profile) ||
      !SpaceDirectory::IsListed(browser->GetProfile())) {
    return nullptr;
  }
  // 1. The profile's next window opens on |space|.
  SpaceService::Get(profile)->space_list().SetActiveSpace(space);
  // 2.
  ui::BaseWindow* old_window = browser->GetWindow();
  BrowserWindowInterface* window =
      CreateSwapWindow(profile, old_window->GetRestoredBounds(),
                       old_window->IsMaximized());
  WindowFeatures* new_features = WindowFeatures::From(window);
  CHECK(&new_features->space_model() == &space);
  // 3.
  new_features->EnterSpace();
  // 4. It covers the old window exactly.
  window->GetWindow()->Show();
  // 5. Once empty, the old window closes on its own.
  features->LeaveForSwap();
  return window;
}

std::vector<SpaceDirectory::Entry> GetSwitchableSpaces(
    BrowserWindowInterface* browser) {
  Profile* profile = browser->GetProfile();
  if (SpaceDirectory* directory = SpaceDirectory::IsListed(profile)
                                      ? SpaceDirectory::Get()
                                      : nullptr) {
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

void ArchiveIdleTodayEntries(Profile* profile, base::Time now) {
  SpaceService* service = SpaceService::Get(profile);
  for (const auto& space : service->space_list().spaces()) {
    space->ArchiveIdleEntries(now, [&](const SidebarNode& entry) {
      return IsEntryInUse(profile, service->registry(), entry);
    });
  }
}

}  // namespace strata
