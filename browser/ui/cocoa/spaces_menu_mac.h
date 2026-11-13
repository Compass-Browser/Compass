// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_COCOA_SPACES_MENU_MAC_H_
#define COMPASS_BROWSER_UI_COCOA_SPACES_MENU_MAC_H_

#import <AppKit/AppKit.h>

#include <string>

namespace compass {

// The Spaces menu's tag in the menu bar.
inline constexpr NSInteger kSpacesMenuTag = 39990;

// The menu bar's Spaces menu (docs/design/spaces.md, "Spaces menu"): the
// spaces the active window can switch to, in the switcher's order, with
// their shortcuts and a check on the window's, then New Space. It's built
// again each time it opens. Nil for a web app's menu bar. Same signature as
// main_menu_builder.mm's builders, which list it (see patches/); defined in
// //compass/browser/ui.
NSMenuItem* BuildSpacesMenu(NSApplication* nsapp,
                            id app_delegate,
                            const std::u16string& product_name,
                            bool is_pwa,
                            bool is_rtl);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_COCOA_SPACES_MENU_MAC_H_
