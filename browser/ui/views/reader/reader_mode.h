// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_READER_READER_MODE_H_
#define COMPASS_BROWSER_UI_VIEWS_READER_READER_MODE_H_

#include <memory>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace tabs {
class TabInterface;
}  // namespace tabs

namespace compass {

class ActiveTabWatcher;
class ReaderTabState;

// A window's Reader mode (docs/design/reader-mode.md): gives the tabs that
// come to the front a ReaderTabState (which checks whether their page is an
// article), puts each tab's reader view over the pane that shows it, keeps
// the sidebar header's Reader button up to date, and runs the Reader
// command. Owned by the window's sidebar view, as Peek.
class ReaderMode {
 public:
  DECLARE_USER_DATA(ReaderMode);

  explicit ReaderMode(BrowserWindowInterface* browser);
  ReaderMode(const ReaderMode&) = delete;
  ReaderMode& operator=(const ReaderMode&) = delete;
  ~ReaderMode();

  static ReaderMode* From(BrowserWindowInterface* browser);

  // For the active tab: its page is an article (the Reader button shows).
  bool IsAvailable() const;
  // The active tab is in Reader.
  bool IsShowing() const;
  // Enters Reader in the active tab, or leaves it. Entering works on any web
  // page; one without an article says so.
  void Toggle();

  // The active tab's state, if it has one.
  ReaderTabState* GetActiveState() const;

 private:
  void OnActiveTabChanged(const tabs::TabInterface* tab);
  void OnReaderChanged(content::WebContents* contents);
  // Puts the reader views over the panes that show tabs in Reader, and
  // updates the header.
  void Sync();
  void OnPaneAttached();

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<ActiveTabWatcher> active_tab_watcher_;
  base::CallbackListSubscription reader_changed_subscription_;
  std::vector<base::CallbackListSubscription> pane_subscriptions_;
  ui::ScopedUnownedUserData<ReaderMode> scoped_data_holder_;
  base::WeakPtrFactory<ReaderMode> weak_factory_{this};
};

// Browser commands for Reader: kToggleReaderModeCommand, and upstream's
// Reading mode commands (IDC_SHOW_READING_MODE, its keyboard twin), which
// open Compass's Reader instead of Chromium's side panel. Returns true if it
// handled |id|. Called from compass::HandleBrowserCommand().
bool HandleReaderCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_READER_READER_MODE_H_
