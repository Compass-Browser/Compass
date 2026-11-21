// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/dev_mode/dev_mode_controller.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/dev_mode/dev_mode.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/ui/compass_command_ids.h"
#include "compass/browser/ui/views/dev_mode/dev_mode_bar.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/views/controls/webview/webview.h"

namespace compass {

DEFINE_USER_DATA(DevModeController);

class DevModeController::PageWatcher : public content::WebContentsObserver {
 public:
  PageWatcher(content::WebContents* contents, base::RepeatingClosure changed)
      : content::WebContentsObserver(contents), changed_(std::move(changed)) {}

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override { changed_.Run(); }

 private:
  base::RepeatingClosure changed_;
};

DevModeController::DevModeController(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  pref_change_registrar_.Init(browser_->GetProfile()->GetPrefs());
  pref_change_registrar_.Add(
      prefs::kDevModeHosts,
      base::BindRepeating(&DevModeController::Sync, base::Unretained(this)));
  // Reports the current tab at once.
  active_tab_watcher_ = std::make_unique<ActiveTabWatcher>(
      browser_, base::BindRepeating(&DevModeController::OnActiveTabChanged,
                                    base::Unretained(this)));
}

DevModeController::~DevModeController() {
  page_watchers_.clear();
  pane_subscriptions_.clear();
  active_tab_watcher_.reset();
  pref_change_registrar_.RemoveAll();
}

// static
DevModeController* DevModeController::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

bool DevModeController::IsEnabledForActiveTab() const {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  return tab && dev_mode::IsEnabledFor(tab->GetContents());
}

bool DevModeController::CanToggleForActiveTab() const {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  return tab && dev_mode::CanEnable(tab->GetContents()->GetLastCommittedURL());
}

void DevModeController::ToggleForActiveTab() {
  tabs::TabInterface* tab = browser_->GetActiveTabInterface();
  if (!tab) {
    return;
  }
  content::WebContents* contents = tab->GetContents();
  dev_mode::SetEnabledFor(
      Profile::FromBrowserContext(contents->GetBrowserContext())->GetPrefs(),
      contents->GetLastCommittedURL(), !dev_mode::IsEnabledFor(contents));
  // The pref observer syncs, for this window and the others.
}

void DevModeController::OnActiveTabChanged(const tabs::TabInterface* tab) {
  Sync();
}

void DevModeController::SyncSoon() {
  // Not while a pane or a page is in the middle of a change.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&DevModeController::Sync, weak_factory_.GetWeakPtr()));
}

void DevModeController::Sync() {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  MultiContentsView* multi =
      browser_view ? browser_view->multi_contents_view() : nullptr;
  if (!multi) {
    return;
  }
  pane_subscriptions_.clear();
  page_watchers_.clear();
  for (ContentsContainerView* container : multi->contents_container_views()) {
    ContentsWebView* contents_view = container->contents_view();
    // A pane that comes to show another tab says so.
    pane_subscriptions_.push_back(
        contents_view->AddWebContentsAttachedCallback(base::BindRepeating(
            [](base::WeakPtr<DevModeController> controller, views::WebView*) {
              if (controller) {
                controller->SyncSoon();
              }
            },
            weak_factory_.GetWeakPtr())));

    content::WebContents* contents = contents_view->web_contents();
    if (contents) {
      page_watchers_.push_back(std::make_unique<PageWatcher>(
          contents, base::BindRepeating(&DevModeController::SyncSoon,
                                        weak_factory_.GetWeakPtr())));
    }
    const bool wanted = contents && dev_mode::IsEnabledFor(contents);
    DevModeBar* bar = DevModeBar::FindIn(container);
    if (wanted) {
      if (!bar) {
        // First, so under everything else in the pane: it overlaps nothing,
        // and the pane wants its capture border last.
        bar = container->AddChildViewAt(
            std::make_unique<DevModeBar>(container), 0);
        container->InvalidateLayout();
      }
      bar->SetWebContents(contents);
    } else if (bar) {
      container->RemoveChildViewT(bar);
      container->InvalidateLayout();
    }
  }
}

bool HandleDevModeCommand(BrowserWindowInterface* browser, int id) {
  if (id != kToggleDevModeCommand) {
    return false;
  }
  if (DevModeController* controller = DevModeController::From(browser)) {
    if (controller->CanToggleForActiveTab()) {
      controller->ToggleForActiveTab();
    }
  }
  return true;
}

}  // namespace compass
