// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/window_features.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/desktop_browser_window_capabilities.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/webui_url_constants.h"
#include "strata/browser/sidebar/sidebar_serializer.h"
#include "strata/browser/window_sync/sidebar_store.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
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

// Whether |browser| is a window the user can see tabs in: not the parking
// window, and not closing.
bool IsUsableNormalWindow(BrowserWindowInterface* browser) {
  WindowFeatures* features = WindowFeatures::From(browser);
  return browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
         !browser->capabilities()->IsAttemptingToCloseBrowser() && features &&
         !features->is_parking_window();
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
      browser_, space_, &registry(), &service_->restored_tab_matcher(*space_));
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
  // With the profile's last window gone, nothing can show the parked tabs.
  bool other_window = false;
  ProfileBrowserCollection::GetForProfile(browser_->GetProfile())
      ->ForEach(
          [&](BrowserWindowInterface* other) {
            other_window = other != browser_ && IsUsableNormalWindow(other);
            return !other_window;
          },
          BrowserCollection::Order::kActivation);
  if (!other_window && service_->parking_window()) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&SpaceService::CloseParkingWindow,
                                  service_->GetWeakPtr()));
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

  // 1. The strip's tabs belong to the space being left.
  std::vector<tabs::TabHandle> leaving;
  for (int i = 0; i < strip->count(); ++i) {
    leaving.push_back(strip->GetTabAtIndex(i)->GetHandle());
  }

  // 2.
  bridge_.reset();
  space_ = &target;

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
      browser_, space_, &registry(), &service_->restored_tab_matcher(target));

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

  // 6. The old space's tabs move out. The window no longer shows any of them,
  // so the bridge puts up no placeholders.
  BrowserWindowInterface* destination = FindOtherWindowShowing(leaving_space);
  for (const tabs::TabHandle& handle : leaving) {
    tabs::TabInterface* tab = handle.Get();
    const int index = tab ? strip->GetIndexOfTab(tab) : TabStripModel::kNoTab;
    if (index == TabStripModel::kNoTab || strip->count() == 1) {
      continue;
    }
    SidebarNode* node = registry().GetNode(tab);
    // Placeholders, and blank Today tabs, which aren't worth keeping.
    if (!node || (leaving_space.today_root()->Contains(node) &&
                  !IsRestorableURL(node->url()))) {
      if (node) {
        registry().Unregister(tab);
        leaving_space.Remove(node);
      }
      strip->CloseWebContentsAt(index, CLOSE_NONE);
      continue;
    }
    if (!destination) {
      destination = service_->GetOrCreateParkingWindow();
    }
    TabStripModel* destination_strip = destination->GetTabStripModel();
    destination_strip->InsertDetachedTabAt(destination_strip->count(),
                                           strip->DetachTabAtForInsertion(index),
                                           AddTabTypes::ADD_NONE);
  }

  service_->space_list().SetActiveSpace(target);
  switching_ = false;
  for (Observer& observer : observers_) {
    observer.OnSpaceSwitched();
  }
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

void MoveEntryToSpace(Profile* profile, SidebarNode* node, SpaceModel& target) {
  SpaceService* service = SpaceService::Get(profile);
  SpaceModel* source = service->space_list().FindSpaceOf(node);
  if (!source || service->space_list().FindSpace(target.id()) != &target) {
    return;
  }
  SidebarNode* new_parent = source->pinned_root()->Contains(node)
                                ? target.pinned_root()
                                : target.today_root();
  if (!source->CanMoveToSpace(node, target, new_parent)) {
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

  source->MoveToSpace(node, target, new_parent, new_parent->children().size());

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

}  // namespace strata
