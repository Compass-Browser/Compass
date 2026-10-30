// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_

#include <initializer_list>
#include <memory>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "components/tabs/public/tab_interface.h"
#include "strata/browser/ui/views/command_bar/command_bar.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/webview.h"

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace views {
class FocusManager;
class View;
}  // namespace views

namespace strata {

// Switches |browser|'s window to its Nth space on Ctrl+N (N = 1 to 9;
// Ctrl+Shift+N outside macOS, where Ctrl+N picks tabs), while registered with
// the window's focus manager. Pages get the keys first.
class SpaceShortcuts : public ui::AcceleratorTarget {
 public:
  explicit SpaceShortcuts(BrowserWindowInterface* browser);
  SpaceShortcuts(const SpaceShortcuts&) = delete;
  SpaceShortcuts& operator=(const SpaceShortcuts&) = delete;
  ~SpaceShortcuts() override;

  void Register(views::FocusManager* focus_manager);
  void Unregister();

  // ui::AcceleratorTarget:
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override;
  bool CanHandleAccelerators() const override;

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<views::FocusManager> focus_manager_ = nullptr;
};

// Hosts chrome://strata-sidebar inside the vertical tab strip region, where
// the native tab strip would be, the window's space shortcuts and its command
// bar. It owns its page, which a switch to another profile's space hands to
// the new window's sidebar (TakePageFrom(), docs/design/profiles-in-place.md):
// the page can be another profile's.
class StrataSidebarView : public views::WebView, public ProfileObserver {
  METADATA_HEADER(StrataSidebarView, views::WebView)

 public:
  explicit StrataSidebarView(BrowserWindowInterface* browser);
  StrataSidebarView(const StrataSidebarView&) = delete;
  StrataSidebarView& operator=(const StrataSidebarView&) = delete;
  ~StrataSidebarView() override;

  BrowserWindowInterface* browser() const { return browser_; }

  // Loads the command bar's page now, unless it has. A swap's window waits
  // until it has taken the old one's place, so it doesn't load at the same
  // time as the sidebar and the page.
  void PreloadCommandBar();

  // Loads a page of the window's profile, unless it has a page. A swap's
  // window starts without one, to take the old window's.
  void LoadPage();
  // Takes |other|'s page, which then shows this window's tree; |other| is
  // left without one.
  void TakePageFrom(StrataSidebarView& other);

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

  // views::WebView:
  void AddedToWidget() override;
  void RemovedFromWidget() override;

 private:
  // Shows |contents| and binds its page to this window.
  void Attach(std::unique_ptr<content::WebContents> contents);
  std::unique_ptr<content::WebContents> Detach();

  const raw_ptr<BrowserWindowInterface> browser_;
  // The page, of the window's profile or, after a switch, of another.
  std::unique_ptr<content::WebContents> contents_;
  // The page's profile, while it isn't the window's: the page goes with it.
  base::ScopedObservation<Profile, ProfileObserver> page_profile_observation_{
      this};
  SpaceShortcuts space_shortcuts_;
  CommandBar command_bar_;
};

// The sidebar the vertical tab strip region shows in place of the native tab
// strip, which Strata never builds. Called from
// VerticalTabStripRegionView::InitializeTabStrip() (see patches/), which adds
// it where the native strip would go.
std::unique_ptr<views::View> CreateSidebar(BrowserWindowInterface* browser);

// Lays out |sidebar| once the region has added it: it takes the free space,
// and |bottom_container| (upstream's New Tab button) is hidden. Tints the
// whole region with the window's space color (SidebarTint) and gives the
// window its frame (WindowFrame).
void InstallSidebar(views::View* sidebar, views::View* bottom_container);

// Opening and closing (docs/design/window-frame.md, "Opening and closing"):
// while the region's width animates, its header and sidebar keep the
// expanded width and slide. How far left they go at region |width|, or 0
// when it isn't animating.
int GetSidebarSlideOffset(int width, int expanded_width, bool animating);

// Fades |views| with the region's |width| while the sidebar closes
// (|closing|), and back to opaque otherwise. They get a layer only for that.
void FadeSidebarContents(std::initializer_list<views::View*> views,
                         int width,
                         int expanded_width,
                         bool closing);

// Reports |browser|'s active tab, as the native strip's node tree did for the
// region: now, whenever it changes, and null as soon as the active tab leaves
// the window. A closing window closes its tabs without changing the active
// one, so without that a listener could keep using a destroyed tab.
class ActiveTabWatcher {
 public:
  using Callback = base::RepeatingCallback<void(const tabs::TabInterface*)>;

  ActiveTabWatcher(BrowserWindowInterface* browser, Callback callback);
  ActiveTabWatcher(const ActiveTabWatcher&) = delete;
  ActiveTabWatcher& operator=(const ActiveTabWatcher&) = delete;
  ~ActiveTabWatcher();

 private:
  void OnActiveTabChanged(BrowserWindowInterface* browser);
  void OnWillDetach(tabs::TabInterface* tab,
                    tabs::TabInterface::DetachReason reason);

  Callback callback_;
  base::CallbackListSubscription active_tab_subscription_;
  base::CallbackListSubscription will_detach_subscription_;
};

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_STRATA_SIDEBAR_VIEW_H_
