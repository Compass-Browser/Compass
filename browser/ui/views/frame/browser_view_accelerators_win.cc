// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/browser_view_accelerators_win.h"

#include "base/no_destructor.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "ui/views/focus/focus_manager.h"

namespace compass {

namespace {

class Follower : public ShortcutSettings::Observer {
 public:
  Follower() { ShortcutSettings::Get().AddObserver(this); }

  // ShortcutSettings::Observer:
  void OnShortcutsChanged() override { Update(); }
  void OnRecordingChanged() override { Update(); }

 private:
  void Update() {
    const bool recording = ShortcutSettings::Get().recording();
    GlobalBrowserCollection::GetInstance()->ForEach(
        [recording](BrowserWindowInterface* browser) {
          if (BrowserView* view =
                  BrowserView::GetBrowserViewForBrowser(browser)) {
            if (recording) {
              BrowserViewAccelerators::Drop(*view);
            } else {
              BrowserViewAccelerators::Reload(*view);
            }
          }
          return true;
        });
  }
};

}  // namespace

// static
void BrowserViewAccelerators::Watch() {
  static base::NoDestructor<Follower> follower;
}

// static
void BrowserViewAccelerators::Reload(BrowserView& view) {
  if (!view.GetFocusManager()) {
    return;
  }
  Drop(view);
  view.LoadAccelerators();
}

// static
void BrowserViewAccelerators::Drop(BrowserView& view) {
  if (views::FocusManager* focus_manager = view.GetFocusManager()) {
    focus_manager->UnregisterAccelerators(&view);
  }
  view.accelerator_table_.clear();
}

}  // namespace compass
