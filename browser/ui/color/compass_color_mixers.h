// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_COLOR_COMPASS_COLOR_MIXERS_H_
#define COMPASS_BROWSER_UI_COLOR_COMPASS_COLOR_MIXERS_H_

namespace ui {
class ColorProvider;
struct ColorProviderKey;
}  // namespace ui

namespace compass {

// Gives views menus (the app menu, comboboxes' lists) the look of Compass's
// bubbles (compass::BubbleLook, docs/design/downloads-bubble.md, "Look"):
// the window's space's tint over the dialog background, a highlight of the
// text color, soft hairline separators. A window's key carries its space's
// tint as its user color, and a menu takes its window's key, so each
// window's menu takes its own space's. Added after upstream's mixers.
void AddCompassColorMixers(ui::ColorProvider* provider,
                           const ui::ColorProviderKey& key);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_COLOR_COMPASS_COLOR_MIXERS_H_
