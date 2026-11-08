// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SPACE_ICON_IMAGE_H_
#define COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SPACE_ICON_IMAGE_H_

#include "ui/base/models/image_model.h"

namespace compass {

class SpaceIcon;

// A space's icon (docs/design/spaces.md, "Space icons") for native UI, such
// as menus: |size| DIP square. A symbol is a vector icon in the menu's icon
// color (a template image in macOS menus, so it follows the appearance); an
// emoji is drawn as it is. Empty for none, and for a symbol this version
// doesn't know (the page shows the name's letter for it).
ui::ImageModel GetSpaceIconImage(const SpaceIcon& icon, int size);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SPACE_ICON_IMAGE_H_
