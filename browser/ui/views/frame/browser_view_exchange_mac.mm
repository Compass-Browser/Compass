// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/browser_view_exchange.h"

#include "chrome/browser/ui/views/frame/browser_native_widget_mac.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"

namespace compass {

// static
void BrowserViewExchange::SetNativeWidgetView(BrowserWidget& widget,
                                              BrowserView* view) {
  auto* native_widget =
      static_cast<BrowserNativeWidgetMac*>(widget.browser_native_widget_.get());
  native_widget->browser_view_ = view;
  native_widget->RebindTouchBar();
}

}  // namespace compass
