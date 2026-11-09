// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/browser_view_exchange.h"

#include <memory>

#include "build/config/linux/dbus/buildflags.h"
#include "chrome/browser/ui/views/frame/browser_desktop_window_tree_host_linux.h"
#include "chrome/browser/ui/views/frame/browser_native_widget_aura.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"

#if BUILDFLAG(USE_DBUS)
#include "chrome/browser/ui/views/frame/dbus_appmenu.h"
#endif

namespace compass {

// As on Windows (browser_view_exchange_win.cc): the native widget and its
// tree host keep the BrowserView they were made for. Left alone, the window
// shown after a switch to another profile's space still points at the view
// that went to the hidden window, which is destroyed when that window
// closes; the tree host reads it on tab drags and fullscreen changes.
void BrowserViewExchange::SetNativeWidgetView(BrowserWidget& widget,
                                              BrowserView* view) {
  auto* native_widget =
      static_cast<BrowserNativeWidgetAura*>(widget.browser_native_widget_.get());
  native_widget->browser_view_ = view;
  auto* host = static_cast<BrowserDesktopWindowTreeHostLinux*>(
      native_widget->browser_desktop_window_tree_host_.get());
  if (!host) {
    return;
  }
  host->browser_view_ = view;
#if BUILDFLAG(USE_DBUS)
  // The global menu bar (KDE, Unity) was made at Init() for the old view's
  // browser: its profile's history, recently closed tabs and commands. It is
  // tied to this native window, so the one for the incoming view is made
  // here, after the old one has unregistered.
  if (host->dbus_appmenu_) {
    host->dbus_appmenu_.reset();
    host->dbus_appmenu_ = std::make_unique<DbusAppmenu>(
        view, host->platform_window(), host->GetAcceleratedWidget());
  }
#endif
}

}  // namespace compass
