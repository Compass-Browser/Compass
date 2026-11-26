// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/bubble_look.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "compass/browser/window_sync/window_features.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/color_utils.h"

namespace compass {

BubbleLook ComputeBubbleLook(const ui::ColorProvider* provider,
                             std::optional<SkColor> tint) {
  BubbleLook look;
  const SkColor base = provider->GetColor(ui::kColorDialogBackground);
  look.dark = color_utils::IsDark(base);
  // The window's own near-black (near-white), not the theme's: the
  // background follows the system (settings-window.md, "Tokens").
  const SkColor fg = look.dark ? SkColorSetRGB(0xEC, 0xEC, 0xEE)
                               : SkColorSetRGB(0x1D, 0x1D, 0x1F);
  look.background =
      tint ? color_utils::AlphaBlend(*tint, base, look.dark ? 0.16f : 0.14f)
           : base;
  look.card = color_utils::AlphaBlend(SK_ColorWHITE, look.background,
                                      look.dark ? 0.06f : 0.72f);
  look.pill = color_utils::AlphaBlend(SK_ColorWHITE, look.background,
                                      look.dark ? 0.15f : 0.94f);
  look.track = color_utils::AlphaBlend(fg, look.background, 0.07f);
  look.hairline = color_utils::AlphaBlend(fg, look.card, 0.10f);
  look.text = fg;
  look.text_dim = color_utils::AlphaBlend(fg, look.background, 0.62f);
  // The space's color, lighter in dark mode so a deep one stays readable;
  // a grey space gets the system's blue.
  look.accent =
      tint ? (look.dark ? color_utils::AlphaBlend(SK_ColorWHITE, *tint, 0.35f)
                        : *tint)
           : (look.dark ? SkColorSetRGB(0x0A, 0x84, 0xFF)
                        : SkColorSetRGB(0x00, 0x7A, 0xFF));
  return look;
}

std::optional<SkColor> GetBubbleTint(BrowserWindowInterface* browser) {
  if (!browser || browser->GetProfile()->IsOffTheRecord()) {
    return std::nullopt;
  }
  WindowFeatures* features = WindowFeatures::From(browser);
  if (!features) {
    return std::nullopt;
  }
  std::optional<SkColor> tint = features->space_model().color().tint();
  if (!tint) {
    return std::nullopt;
  }
  return ToTintColor(*tint, TintSurface::kPane);
}

}  // namespace compass
