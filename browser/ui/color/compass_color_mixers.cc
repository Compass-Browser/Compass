// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/color/compass_color_mixers.h"

#include <optional>

#include "base/functional/bind.h"
#include "ui/color/color_id.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/color/color_recipe.h"
#include "ui/color/color_transform.h"
#include "ui/gfx/color_utils.h"

namespace compass {

namespace {

// BubbleLook's (browser/ui/views/bubble_look.cc): the window's own
// near-black or near-white text, and the tint's share of the background.
constexpr SkColor kDarkText = SkColorSetRGB(0xEC, 0xEC, 0xEE);
constexpr SkColor kLightText = SkColorSetRGB(0x1D, 0x1D, 0x1F);

SkColor Background(std::optional<SkColor> tint, const ui::ColorMixer& mixer) {
  const SkColor base = mixer.GetResultColor(ui::kColorDialogBackground);
  if (!tint) {
    return base;
  }
  return color_utils::AlphaBlend(*tint, base,
                                 color_utils::IsDark(base) ? 0.16f : 0.14f);
}

SkColor Text(const ui::ColorMixer& mixer) {
  return color_utils::IsDark(mixer.GetResultColor(ui::kColorDialogBackground))
             ? kDarkText
             : kLightText;
}

// The text color at |alpha| over the background.
ui::ColorTransform TextOver(std::optional<SkColor> tint, float alpha) {
  return base::BindRepeating(
      [](std::optional<SkColor> tint, float alpha, SkColor,
         const ui::ColorMixer& mixer) {
        return color_utils::AlphaBlend(Text(mixer), Background(tint, mixer),
                                       alpha);
      },
      tint, alpha);
}

}  // namespace

void AddCompassColorMixers(ui::ColorProvider* provider,
                           const ui::ColorProviderKey& key) {
  ui::ColorMixer& mixer = provider->AddPostprocessingMixer();
  const std::optional<SkColor> tint = key.user_color;

  mixer[ui::kColorMenuBackground] = ui::ColorTransform(base::BindRepeating(
      [](std::optional<SkColor> tint, SkColor, const ui::ColorMixer& mixer) {
        return Background(tint, mixer);
      },
      tint));
  // The sidebar's and the bubbles' rows: the text color over the
  // background, so it shows on a tinted, dark one too.
  mixer[ui::kColorMenuItemBackgroundSelected] = TextOver(tint, 0.10f);
  mixer[ui::kColorMenuItemBackgroundHighlighted] = TextOver(tint, 0.06f);
  mixer[ui::kColorMenuSeparator] = TextOver(tint, 0.10f);
  mixer[ui::kColorMenuBorder] = TextOver(tint, 0.12f);
  mixer[ui::kColorMenuItemForeground] = ui::ColorTransform(base::BindRepeating(
      [](SkColor, const ui::ColorMixer& mixer) { return Text(mixer); }));
  mixer[ui::kColorMenuItemForegroundHighlighted] = {
      ui::kColorMenuItemForeground};
  mixer[ui::kColorMenuItemForegroundSelected] = {ui::kColorMenuItemForeground};
  mixer[ui::kColorMenuItemForegroundSecondary] = TextOver(tint, 0.62f);
  mixer[ui::kColorMenuIcon] = TextOver(tint, 0.78f);
}

}  // namespace compass
