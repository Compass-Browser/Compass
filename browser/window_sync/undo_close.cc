// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/undo_close.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/uuid.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/core/session_id.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/sidebar/closed_entries.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"

namespace compass {

namespace {

// |browser|'s features, if it's a window of the sidebar (not the parking
// window, a little window, or one switching spaces).
WindowFeatures* GetSidebarWindow(BrowserWindowInterface* browser) {
  WindowFeatures* features = browser ? WindowFeatures::From(browser) : nullptr;
  if (!features || features->is_parking_window() || features->is_switching() ||
      !features->has_tab_strip_bridge()) {
    return nullptr;
  }
  return features;
}

// The space |step| was in, or, if that's gone, the one |features| shows.
SpaceModel& GetSpace(WindowFeatures& features,
                     const ClosedEntries::Step& step) {
  SpaceModel* space =
      step.space_id.is_valid()
          ? features.service().space_list().FindSpace(step.space_id)
          : nullptr;
  return space ? *space : features.space_model();
}

}  // namespace

bool CanUndoClose(BrowserWindowInterface* browser) {
  WindowFeatures* features = GetSidebarWindow(browser);
  return features && features->service().closed_entries().CanUndo();
}

bool CanRedoClose(BrowserWindowInterface* browser) {
  WindowFeatures* features = GetSidebarWindow(browser);
  return features && features->service().closed_entries().CanRedo();
}

bool UndoClose(BrowserWindowInterface* browser) {
  WindowFeatures* features = GetSidebarWindow(browser);
  if (!features) {
    return false;
  }
  SpaceService& service = features->service();
  ClosedEntries& closed = service.closed_entries();
  std::optional<ClosedEntries::Step> step = closed.TakeUndo();
  if (!step) {
    return false;
  }

  // The window shows the space the entries were in. Favorites are in every
  // space.
  SpaceModel& space = GetSpace(*features, *step);
  if (&space != &features->space_model()) {
    browser = SwitchWindowToSpace(browser, browser->GetProfile(), space);
    features = GetSidebarWindow(browser);
  }

  LiveTabRegistry& registry = service.registry();
  SidebarNode* reopen = ClosedEntries::Restore(
      *step, space, service.space_list().favorites(),
      [&registry](const SidebarNode& node) {
        return registry.GetTab(&node) != nullptr;
      });

  // Only the tab that was shown (or the first that was open) opens; the
  // rest come back unloaded.
  if (reopen && features && &features->space_model() == &space) {
    std::optional<SessionID> history;
    if (auto it = step->history.find(reopen->id()); it != step->history.end()) {
      history = SessionID::FromSerializedValue(it->second);
    }
    if (tabs::TabInterface* tab =
            features->tab_strip_bridge().ReopenNode(reopen, history)) {
      TabStripModel* strip = browser->GetTabStripModel();
      const int index = strip->GetIndexOfTab(tab);
      if (index != TabStripModel::kNoTab) {
        strip->ActivateTabAt(index);
      }
    }
  }
  closed.PushRedo(std::move(*step));
  return true;
}

bool RedoClose(BrowserWindowInterface* browser) {
  WindowFeatures* features = GetSidebarWindow(browser);
  if (!features) {
    return false;
  }
  SpaceService& service = features->service();
  ClosedEntries& closed = service.closed_entries();
  std::optional<ClosedEntries::Step> step = closed.TakeRedo();
  if (!step) {
    return false;
  }
  SpaceModel& space = GetSpace(*features, *step);
  SpaceModel& favorites = service.space_list().favorites();
  LiveTabRegistry& registry = service.registry();

  // Closing them again is a new step to undo, recorded as the sidebar
  // records its closes: everything first, so that it comes back in order,
  // then the closes.
  ClosedEntries::ScopedStep scoped(closed, /*keep_redo=*/true);
  struct Close {
    raw_ptr<SpaceModel> model;
    base::Uuid id;
    bool removes;
  };
  std::vector<Close> closes;
  for (const ClosedEntries::Entry& entry : step->entries) {
    SpaceModel& model = entry.favorites ? favorites : space;
    SidebarNode* node = model.FindById(entry.id);
    if (!node) {
      continue;
    }
    if (entry.node) {
      closed.RecordRemoval(model, *node);
      ExpectLiveTabsToClose(registry, *node, closed);
      closes.push_back({&model, entry.id, /*removes=*/true});
    } else if (node->type() == SidebarNode::Type::kTab &&
               registry.GetTab(node)) {
      // A tab that stayed: only its tab closes.
      closed.RecordUnload(model, *node);
      closed.ExpectClose(node->id());
      closes.push_back({&model, entry.id, /*removes=*/false});
    }
  }
  for (const Close& close : closes) {
    SidebarNode* node = close.model->FindById(close.id);
    if (!node) {
      continue;
    }
    if (close.removes) {
      // Its live tabs close with it (TabStripBridge::OnNodeWillBeRemoved()).
      close.model->Remove(node);
    } else if (!CloseLiveTab(registry, *node)) {
      closed.TakeExpectedClose(close.id);
    }
  }
  return true;
}

bool CloseLiveTab(LiveTabRegistry& registry, const SidebarNode& tab_node) {
  tabs::TabInterface* tab = registry.GetTab(&tab_node);
  BrowserWindowInterface* window =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  WindowFeatures* features = window ? WindowFeatures::From(window) : nullptr;
  if (!features || features->is_parking_window() ||
      !features->has_tab_strip_bridge()) {
    return false;
  }
  TabStripModel* strip = window->GetTabStripModel();
  const int index = strip->GetIndexOfTab(tab);
  if (index == TabStripModel::kNoTab) {
    return false;
  }
  strip->CloseWebContentsAt(index,
                            CLOSE_USER_GESTURE | CLOSE_CREATE_HISTORICAL_TAB);
  return true;
}

void ExpectLiveTabsToClose(LiveTabRegistry& registry,
                           const SidebarNode& node,
                           ClosedEntries& closed) {
  if (registry.GetTab(&node)) {
    closed.ExpectClose(node.id());
  }
  for (const auto& child : node.children()) {
    ExpectLiveTabsToClose(registry, *child, closed);
  }
}

}  // namespace compass
