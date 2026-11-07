// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/browser_view_exchange.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/views/frame/browser_desktop_window_tree_host_win.h"
#include "chrome/browser/ui/views/frame/browser_native_widget_aura.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "chrome/browser/ui/views/frame/browser_window_property_manager_win.h"

namespace compass {

// As on macOS (browser_view_exchange_mac.mm): the native widget, and here its
// tree host too, keep the BrowserView they were made for. Left alone, the
// window shown after a switch to another profile's space still points at the
// view that went to the hidden window, which is destroyed when that window
// closes; the next WM_ERASEBKGND (GetDwmFrameInsetsInPixels()) then reads
// it and crashes.
void BrowserViewExchange::SetNativeWidgetView(BrowserWidget& widget,
                                              BrowserView* view) {
  auto* native_widget =
      static_cast<BrowserNativeWidgetAura*>(widget.browser_native_widget_.get());
  native_widget->browser_view_ = view;
  auto* host = static_cast<BrowserDesktopWindowTreeHostWin*>(
      native_widget->browser_desktop_window_tree_host_.get());
  if (!host) {
    return;
  }
  host->browser_view_ = view;
  // Made for the old view at HandleCreate(): the window's taskbar relaunch
  // command and icon, which name its profile.
  host->browser_window_property_manager_.reset();
  host->browser_window_property_manager_ =
      BrowserWindowPropertyManager::CreateBrowserWindowPropertyManager(
          view, host->GetHWND());
  // With more than one profile, the window icon carries the profile's
  // avatar.
  if (g_browser_process->profile_manager()
          ->GetProfileAttributesStorage()
          .GetNumberOfProfiles() > 1) {
    host->SetWindowIcon(/*badged=*/true);
  }
}

}  // namespace compass
