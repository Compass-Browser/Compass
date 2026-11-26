// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_FRAME_CONTENT_FRAME_H_
#define COMPASS_BROWSER_UI_VIEWS_FRAME_CONTENT_FRAME_H_

#include "ui/gfx/geometry/insets.h"

namespace views {
class View;
}  // namespace views

namespace compass {

// As in Arc, the page sits in a rounded card inside a frame of the space's
// color: the window's edges and the sidebar. See docs/design/window-frame.md.

// How far the card is from the window's top, trailing and bottom edges.
inline constexpr int kContentFrameInset = 8;

// The card's corner radius. With the inset, concentric with macOS's window
// corners.
inline constexpr float kContentCornerRadius = 12.f;

// A collapsed sidebar is only the frame's leading edge, where hovering shows
// the sidebar over the page.
inline constexpr int kCollapsedSidebarWidth = kContentFrameInset;

// The card's insets in the area beside the sidebar, which sets the leading
// edge. |trailing_caption_height| is how far down the window's caption
// buttons reach at its top trailing corner (Windows, Linux; 0 on macOS,
// where the traffic lights are in the sidebar): the card starts below them,
// as they paint over the frame (docs/design/window-frame.md, "Windows").
inline constexpr gfx::Insets GetContentFrameInsets(
    int trailing_caption_height = 0) {
  return gfx::Insets::TLBR(
      trailing_caption_height > kContentFrameInset ? trailing_caption_height
                                                   : kContentFrameInset,
      0, kContentFrameInset, kContentFrameInset);
}

// The card's insets in a popup's |browser_view| under its toolbar: the
// frame's in a little window (docs/design/little-window.md), none in other
// popups. Upstream's popup layout asks (see patches/); defined with the
// little windows, in //compass/browser/ui.
gfx::Insets GetPopupContentFrameInsets(const views::View* browser_view);

// The height a bar at the top of a pane takes from the page and its docked
// DevTools: development mode's bar (docs/design/dev-mode.md), 0 without
// one. |container| is a ContentsContainerView; upstream's layout of it asks
// (see patches/). Defined with the bar, in //compass/browser/ui.
int GetContentsTopBarHeight(const views::View* container);

// Whether the frame of |browser_view|'s window is translucent now
// (docs/design/spaces.md, "Translucency"): the frame view, the region's
// background (unless it shows on hover over the page) and the card's corners
// don't paint the frame color, the window and its compositor aren't opaque,
// and the frame's tint paints the frame color only partly over the window's
// backdrop. Not in fullscreen. Upstream's layout and frame views ask (see
// patches/); defined with the window frame, in //compass/browser/ui.
bool IsFrameTranslucent(const views::View* browser_view);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_FRAME_CONTENT_FRAME_H_
