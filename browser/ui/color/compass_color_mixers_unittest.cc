// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/color/compass_color_mixers.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "ui/color/color_id.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/color/color_recipe.h"
#include "ui/color/color_transform.h"
#include "ui/gfx/color_utils.h"

namespace compass {
namespace {

// A provider with only a dialog background, then Compass's mixers.
SkColor MenuColor(SkColor dialog,
                  std::optional<SkColor> tint,
                  ui::ColorId id) {
  ui::ColorProvider provider;
  provider.AddMixer()[ui::kColorDialogBackground] = ui::ColorTransform(dialog);
  ui::ColorProviderKey key;
  key.user_color = tint;
  AddCompassColorMixers(&provider, key);
  return provider.GetColor(id);
}

TEST(CompassColorMixersTest, MenuTakesTheSpacesTint) {
  const SkColor white = SK_ColorWHITE;
  EXPECT_EQ(MenuColor(white, std::nullopt, ui::kColorMenuBackground), white);
  const SkColor tinted =
      MenuColor(white, SK_ColorBLUE, ui::kColorMenuBackground);
  EXPECT_NE(tinted, white);
  EXPECT_GT(SkColorGetB(tinted), SkColorGetR(tinted));
}

TEST(CompassColorMixersTest, HighlightShowsOnDarkMenus) {
  const SkColor dark = SkColorSetRGB(0x20, 0x20, 0x22);
  const SkColor background =
      MenuColor(dark, std::nullopt, ui::kColorMenuBackground);
  const SkColor selected =
      MenuColor(dark, std::nullopt, ui::kColorMenuItemBackgroundSelected);
  // Lighter than the menu on a dark one, as the bubbles' hovered rows.
  EXPECT_GT(color_utils::GetRelativeLuminance(selected),
            color_utils::GetRelativeLuminance(background));
  EXPECT_TRUE(color_utils::IsDark(
      MenuColor(SK_ColorWHITE, std::nullopt, ui::kColorMenuItemForeground)));
}

}  // namespace
}  // namespace compass
