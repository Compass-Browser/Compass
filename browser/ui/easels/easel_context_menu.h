// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_EASELS_EASEL_CONTEXT_MENU_H_
#define COMPASS_BROWSER_UI_EASELS_EASEL_CONTEXT_MENU_H_

namespace content {
class WebContents;
}  // namespace content

namespace ui {
class SimpleMenuModel;
}  // namespace ui

// Compass's items in a page's context menu: "Capture to Easel…"
// (docs/design/easels.md, "Captures"). Upstream's RenderViewContextMenu
// calls these (patches/
// chrome-browser-renderer_context_menu-render_view_context_menu.cc.patch).
//
// Declarations only, in //compass/browser/ui/easels:context_menu_hooks, so
// upstream's target can include them without depending on Compass's UI;
// the definitions are in //compass/browser/ui/easels:capture, linked with
// //chrome/browser/ui.
namespace compass::easels {

// Outside upstream's command IDs and Compass's browser commands
// (compass_command_ids.h).
inline constexpr int kCaptureToEaselContextCommand = 39970;

// Adds Compass's items for |contents|' page, after the page's own (Back,
// Reload, Save, Print...), when they apply.
void AppendPageContextMenuItems(content::WebContents* contents,
                                ui::SimpleMenuModel* model);

// Whether |id| is one of those items' (always enabled when shown).
bool IsCompassContextMenuCommand(int id);

// Runs |id| for |contents|; false if it isn't Compass's. Runs before
// upstream's handling, which would take an unknown ID for a bug.
bool ExecuteCompassContextMenuCommand(content::WebContents* contents, int id);

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_UI_EASELS_EASEL_CONTEXT_MENU_H_
