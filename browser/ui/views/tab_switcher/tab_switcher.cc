// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/tab_switcher/tab_switcher.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/tab_switcher/recent_entries.h"
#include "compass/browser/ui/views/tab_switcher/tab_switcher_view.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/undo_close.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

bool g_control_held_for_testing = false;

bool ControlHeld() {
  return g_control_held_for_testing || SwitcherKeys::IsControlDown();
}

SpaceModel& Favorites(WindowFeatures& features) {
  return features.service().space_list().favorites();
}

}  // namespace

DEFINE_USER_DATA(TabSwitcher);

TabSwitcher::TabSwitcher(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  SwitcherKeys::Track();
}

TabSwitcher::~TabSwitcher() {
  End();
}

// static
TabSwitcher* TabSwitcher::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

// static
void TabSwitcher::SetControlHeldForTesting(bool held) {
  g_control_held_for_testing = held;
}

bool TabSwitcher::IsEnabled() const {
  return browser_->GetProfile()->GetPrefs()->GetBoolean(prefs::kCtrlTabRecent);
}

bool TabSwitcher::Step(int step) {
  if (IsActive() && !ControlHeld()) {
    // Ctrl went up where the keys weren't watched (another app, say): that
    // switch is over, this is a new one.
    Commit();
  }
  if (IsActive()) {
    OnMove(step);
    return true;
  }
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (!features || !features->has_tab_strip_bridge()) {
    return false;
  }
  const SidebarNode* shown = features->tab_strip_bridge().GetShownNode(
      browser_->GetActiveTabInterface());
  std::vector<RecentEntry> recent = GetRecentEntries(
      features->space_model(), Favorites(*features), shown);
  if (recent.size() < 2) {
    return false;
  }
  if (recent.size() > kMaxEntries) {
    recent.resize(kMaxEntries);
  }
  for (const RecentEntry& e : recent) {
    entries_.push_back({e.tab->id(), e.favorite});
  }
  // The previous entry, or the oldest going the other way.
  selected_ = step > 0 ? 1 : entries_.size() - 1;
  if (!ControlHeld()) {
    // Ctrl is up already (a test, or a very quick tap): switch.
    Commit();
    return true;
  }
  keys_ = SwitcherKeys::Watch(this);
  show_timer_.Start(FROM_HERE, kShowDelay,
                    base::BindOnce(&TabSwitcher::Show,
                                   weak_factory_.GetWeakPtr()));
  return true;
}

void TabSwitcher::Show() {
  if (IsActive() && !ControlHeld()) {
    Commit();  // As in Step().
    return;
  }
  WindowFeatures* features = WindowFeatures::From(browser_);
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  views::Widget* widget = browser_view ? browser_view->GetWidget() : nullptr;
  if (!features || !widget || !IsActive()) {
    return;
  }
  std::vector<TabSwitcherView::Card> cards;
  for (const Entry& entry : entries_) {
    SpaceModel& model = entry.favorite ? Favorites(*features)
                                       : features->space_model();
    SidebarNode* node = model.FindById(entry.tab);
    TabSwitcherView::Card card;
    if (tabs::TabInterface* tab =
            node ? features->registry().GetTab(node) : nullptr) {
      tabs::TabData data = tabs::TabData::FromTabInterface(tab);
      card.title = node->custom_title().empty() ? data.title
                                                : node->custom_title();
      card.favicon = data.favicon;
      card.thumbnail = data.thumbnail;
    } else if (node) {
      card.title = node->custom_title().empty() ? node->title()
                                                : node->custom_title();
    }
    if (card.favicon.IsEmpty()) {
      card.favicon = ui::ImageModel::FromVectorIcon(
          vector_icons::kGlobeIcon, ui::kColorIcon, 16);
    }
    cards.push_back(std::move(card));
  }
  bubble_ = TabSwitcherView::Show(
      widget, std::move(cards), selected_,
      base::BindRepeating(&TabSwitcher::Pick, weak_factory_.GetWeakPtr()),
      // The cards went with their window: the switch ends there.
      base::BindOnce(&TabSwitcher::End, bubble_weak_factory_.GetWeakPtr()));
}

TabSwitcherView* TabSwitcher::view_for_testing() {
  return bubble_ ? bubble_->view.get() : nullptr;
}

void TabSwitcher::OnMove(int step) {
  if (!IsActive()) {
    return;
  }
  const size_t count = entries_.size();
  selected_ = (selected_ + count + (step % static_cast<int>(count))) % count;
  if (bubble_) {
    bubble_->view->SetSelected(selected_);
  }
}

void TabSwitcher::Pick(size_t index) {
  if (index < entries_.size()) {
    selected_ = index;
    Commit();
  }
}

void TabSwitcher::Commit() {
  if (!IsActive()) {
    return;
  }
  const Entry picked = entries_[selected_];
  End();
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (!features || !features->has_tab_strip_bridge()) {
    return;
  }
  SpaceModel& model =
      picked.favorite ? Favorites(*features) : features->space_model();
  SidebarNode* node = model.FindById(picked.tab);
  if (!node) {
    return;  // Closed meanwhile.
  }
  // Opening an unloaded entry activates its new tab, as the sidebar does.
  if (tabs::TabInterface* tab = features->tab_strip_bridge().OpenNode(node)) {
    TabStripModel* strip = browser_->GetTabStripModel();
    strip->ActivateTabAt(strip->GetIndexOfTab(tab));
  }
}

void TabSwitcher::Cancel() {
  End();
}

void TabSwitcher::OnControlReleased() {
  Commit();
}

void TabSwitcher::OnCancel() {
  Cancel();
}

void TabSwitcher::OnCommit() {
  Commit();
}

void TabSwitcher::OnClose() {
  CloseSelected();
}

void TabSwitcher::CloseSelected() {
  if (!IsActive()) {
    return;
  }
  WindowFeatures* features = WindowFeatures::From(browser_);
  if (!features) {
    return;
  }
  const Entry picked = entries_[selected_];
  entries_.erase(entries_.begin() + selected_);
  SpaceModel& model =
      picked.favorite ? Favorites(*features) : features->space_model();
  if (SidebarNode* node = model.FindById(picked.tab)) {
    CloseSidebarEntry(*features, node);
  }
  // Closing may have closed others too (the last tab of the window's
  // space closing it, say): keep only what's still there.
  std::erase_if(entries_, [&](const Entry& entry) {
    SpaceModel& m =
        entry.favorite ? Favorites(*features) : features->space_model();
    return !m.FindById(entry.tab);
  });
  if (entries_.empty()) {
    End();
    return;
  }
  selected_ = std::min(selected_, entries_.size() - 1);
  if (bubble_) {
    // The cards again, without the closed one.
    bubble_weak_factory_.InvalidateWeakPtrs();
    std::unique_ptr<TabSwitcherBubble> bubble = std::move(bubble_);
    bubble.reset();
    Show();
  }
}

void TabSwitcher::End() {
  show_timer_.Stop();
  keys_.reset();
  entries_.clear();
  selected_ = 0;
  // Moved out first: destroying it may run its closed callback, End().
  std::unique_ptr<TabSwitcherBubble> bubble = std::move(bubble_);
  bubble.reset();
}

bool HandleTabSwitcherCommand(BrowserWindowInterface* browser, int id) {
  // Ctrl+Tab and Ctrl+Shift+Tab (accelerator_table.cc); Next and Previous
  // tab's other keys stay in sidebar order.
  if (id != IDC_CYCLE_TO_NEXT_TAB && id != IDC_CYCLE_TO_PREV_TAB) {
    return false;
  }
  TabSwitcher* switcher = TabSwitcher::From(browser);
  if (!switcher || !switcher->IsEnabled()) {
    return false;
  }
  return switcher->Step(id == IDC_CYCLE_TO_NEXT_TAB ? 1 : -1);
}

}  // namespace compass
