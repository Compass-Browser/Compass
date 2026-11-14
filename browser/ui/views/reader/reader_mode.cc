// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/reader/reader_mode.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/views/reader/reader_tab_state.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
#include "compass/browser/ui/views/sidebar/sidebar_header.h"
#include "content/public/browser/web_contents.h"
#include "ui/views/controls/webview/webview.h"

namespace compass {

DEFINE_USER_DATA(ReaderMode);

ReaderMode::ReaderMode(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  reader_changed_subscription_ = ReaderTabState::RegisterChanged(
      base::BindRepeating(&ReaderMode::OnReaderChanged,
                          base::Unretained(this)));
  // Reports the current tab at once.
  active_tab_watcher_ = std::make_unique<ActiveTabWatcher>(
      browser_, base::BindRepeating(&ReaderMode::OnActiveTabChanged,
                                    base::Unretained(this)));
}

ReaderMode::~ReaderMode() {
  pane_subscriptions_.clear();
  active_tab_watcher_.reset();
  reader_changed_subscription_ = {};
}

// static
ReaderMode* ReaderMode::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

ReaderTabState* ReaderMode::GetActiveState() const {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  return tab ? ReaderTabState::FromWebContents(tab->GetContents()) : nullptr;
}

bool ReaderMode::IsAvailable() const {
  ReaderTabState* state = GetActiveState();
  return state && (state->readerable() || state->showing());
}

bool ReaderMode::IsShowing() const {
  ReaderTabState* state = GetActiveState();
  return state && state->showing();
}

void ReaderMode::Toggle() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  if (!tab) {
    return;
  }
  ReaderTabState::CreateForWebContents(tab->GetContents());
  ReaderTabState* state = ReaderTabState::FromWebContents(tab->GetContents());
  if (state->showing()) {
    state->Leave();
  } else {
    state->Enter();
  }
}

void ReaderMode::OnActiveTabChanged(const tabs::TabInterface* tab) {
  if (tab) {
    // Checks the page from now on.
    ReaderTabState::CreateForWebContents(
        const_cast<tabs::TabInterface*>(tab)->GetContents());
  }
  Sync();
}

void ReaderMode::OnReaderChanged(content::WebContents* contents) {
  if (browser_->GetTabStripModel()->GetIndexOfWebContents(contents) !=
      TabStripModel::kNoTab) {
    Sync();
  }
}

void ReaderMode::Sync() {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  MultiContentsView* multi =
      browser_view ? browser_view->multi_contents_view() : nullptr;
  if (multi) {
    // Each pane that shows a tab in Reader gets its view; a pane that comes
    // to show one later says so.
    pane_subscriptions_.clear();
    for (ContentsContainerView* container :
         multi->contents_container_views()) {
      ContentsWebView* contents_view = container->contents_view();
      pane_subscriptions_.push_back(
          contents_view->AddWebContentsAttachedCallback(base::BindRepeating(
              [](base::WeakPtr<ReaderMode> reader_mode, views::WebView*) {
                // Not while the pane is in the middle of the change.
                base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                    FROM_HERE, base::BindOnce(&ReaderMode::OnPaneAttached,
                                              reader_mode));
              },
              weak_factory_.GetWeakPtr())));
      content::WebContents* contents = contents_view->web_contents();
      ReaderTabState* state =
          contents ? ReaderTabState::FromWebContents(contents) : nullptr;
      if (state && state->showing()) {
        state->AttachTo(container);
      }
    }
  }
  if (SidebarHeader* header = SidebarHeader::From(browser_)) {
    header->UpdateReaderButton();
  }
}

void ReaderMode::OnPaneAttached() {
  Sync();
}

bool HandleReaderCommand(BrowserWindowInterface* browser, int id) {
  if (id != kToggleReaderModeCommand && id != IDC_SHOW_READING_MODE &&
      id != IDC_SHOW_READING_MODE_KEYBOARD) {
    return false;
  }
  // Chromium's Reading mode never opens, also where there's no Reader (a
  // little window).
  if (ReaderMode* reader_mode = ReaderMode::From(browser)) {
    reader_mode->Toggle();
  }
  return true;
}

}  // namespace compass
