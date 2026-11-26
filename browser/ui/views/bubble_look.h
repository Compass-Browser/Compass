// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_BUBBLE_LOOK_H_
#define COMPASS_BROWSER_UI_VIEWS_BUBBLE_LOOK_H_

#include <optional>

#include "third_party/skia/include/core/SkColor.h"

class BrowserWindowInterface;

namespace ui {
class ColorProvider;
}  // namespace ui

namespace compass {

// The colors of Compass's bubbles (the downloads bubble, the share bubble),
// from the dialog's background and the window's space tint: as the settings
// window (settings-window.md, "Visual design"), the tint washes the
// background, and what sits on it is translucent white.
struct BubbleLook {
  SkColor background = SK_ColorWHITE;
  SkColor card = SK_ColorWHITE;
  SkColor pill = SK_ColorWHITE;
  SkColor track = SK_ColorWHITE;
  SkColor hairline = SK_ColorBLACK;
  SkColor text = SK_ColorBLACK;
  SkColor text_dim = SK_ColorBLACK;
  SkColor accent = SK_ColorBLUE;
  bool dark = false;
};

BubbleLook ComputeBubbleLook(const ui::ColorProvider* provider,
                             std::optional<SkColor> tint);

// The window's space's color, which its bubbles take: none for a grey
// space, or off the record (as the sidebar's tint).
std::optional<SkColor> GetBubbleTint(BrowserWindowInterface* browser);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_BUBBLE_LOOK_H_
