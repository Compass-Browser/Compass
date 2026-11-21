// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_TAB_SWITCHER_H_
#define COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_TAB_SWITCHER_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "compass/browser/ui/views/tab_switcher/switcher_keys.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace compass {

struct TabSwitcherBubble;
class TabSwitcherView;

// Ctrl+Tab in recently used order (docs/design/sidebar.md, "Recent tabs"),
// as Arc: the window's entries (its space and the favorites) by when they
// were last shown. Ctrl+Tab picks the previous one, again goes further back
// (Ctrl+Shift+Tab, forward), and releasing Ctrl switches; held a moment, a
// row of cards shows where it is. Esc goes back. Owned by the window's
// sidebar view, as DevModeController.
class TabSwitcher : public SwitcherKeys::Delegate {
 public:
  DECLARE_USER_DATA(TabSwitcher);

  // At most this many entries (the row of cards then fits the window by
  // shrinking them, and sliding).
  static constexpr size_t kMaxEntries = 8;
  // A Ctrl+Tab tap switches without showing the cards.
  static constexpr base::TimeDelta kShowDelay = base::Milliseconds(150);

  explicit TabSwitcher(BrowserWindowInterface* browser);
  TabSwitcher(const TabSwitcher&) = delete;
  TabSwitcher& operator=(const TabSwitcher&) = delete;
  ~TabSwitcher() override;

  static TabSwitcher* From(BrowserWindowInterface* browser);

  // Whether Ctrl+Tab goes in recently used order, for this window's profile
  // (prefs::kCtrlTabRecent).
  bool IsEnabled() const;

  // Ctrl+Tab (|step| 1) or Ctrl+Shift+Tab (-1): starts the switcher, or
  // moves it. False when there's nothing to switch to (the caller does
  // what the keys would otherwise do).
  bool Step(int step);
  // Switches to the picked entry, or goes back, and ends.
  void Commit();
  void Cancel();

  bool IsActive() const { return !entries_.empty(); }
  // Tests can't hold Ctrl: Step() acts as if it were held.
  static void SetControlHeldForTesting(bool held);
  bool IsShowingForTesting() const { return !!bubble_; }
  TabSwitcherView* view_for_testing();
  size_t selected_for_testing() const { return selected_; }
  size_t entry_count_for_testing() const { return entries_.size(); }

  // SwitcherKeys::Delegate:
  void OnControlReleased() override;
  void OnCancel() override;
  void OnCommit() override;
  void OnMove(int step) override;

 private:
  struct Entry {
    base::Uuid tab;
    bool favorite = false;
  };

  void Show();
  void Pick(size_t index);
  void End();

  const raw_ptr<BrowserWindowInterface> browser_;
  std::vector<Entry> entries_;
  size_t selected_ = 0;
  std::unique_ptr<SwitcherKeys> keys_;
  base::OneShotTimer show_timer_;
  std::unique_ptr<TabSwitcherBubble> bubble_;
  ui::ScopedUnownedUserData<TabSwitcher> scoped_data_holder_;
  base::WeakPtrFactory<TabSwitcher> weak_factory_{this};
};

// Runs Ctrl+Tab and Ctrl+Shift+Tab (IDC_CYCLE_TO_NEXT_TAB and _PREV_TAB) as
// the recent tabs switcher while the setting is on. Called from
// compass::HandleBrowserCommand().
bool HandleTabSwitcherCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_TAB_SWITCHER_H_
