// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>

#include <dwmapi.h>

#include "base/logging.h"
#include "compass/browser/ui/views/frame/window_frame.h"
#include "components/viz/common/features.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/color/color_provider_key.h"
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/widget.h"

namespace compass {

bool IsWindowBackdropSupported() {
  // Windows 11 22H2 and later, with DirectComposition: the window has no
  // redirection bitmap, and DWM draws a system backdrop (Mica) under what the
  // compositor draws, which a transparent compositor lets show.
  return features::ShouldRemoveRedirectionBitmap();
}

void SetWindowBackdrop(views::Widget* widget, bool on) {
  if (!IsWindowBackdropSupported()) {
    return;
  }
  aura::Window* window = widget->GetNativeWindow();
  if (!window || !window->GetHost()) {
    return;
  }
  // Acrylic shows the windows behind, blurred; Mica, upstream's, only the
  // wallpaper's color.
  const HWND hwnd = window->GetHost()->GetAcceleratedWidget();
  const DWM_SYSTEMBACKDROP_TYPE type =
      on ? DWMSBT_TRANSIENTWINDOW : DWMSBT_MAINWINDOW;
  HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type,
                                     sizeof(type));
  if (FAILED(hr)) {
    LOG(ERROR) << "DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE) failed: "
               << hr;
  }
  // The backdrop in the window's mode, which a space can set apart from the
  // system's.
  const BOOL dark =
      widget->GetColorMode() == ui::ColorProviderKey::ColorMode::kDark;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  // The compositor clears to transparent where the browser window tree host
  // asks for it (IsFrameTranslucent(), patch in
  // BrowserDesktopWindowTreeHostWin::ShouldWindowContentsBeTransparent()).
  if (auto* native_widget =
          views::DesktopNativeWidgetAura::ForWindow(window)) {
    native_widget->UpdateWindowTransparency();
  }
}

}  // namespace compass
