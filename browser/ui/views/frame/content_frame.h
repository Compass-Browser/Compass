// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_FRAME_CONTENT_FRAME_H_
#define STRATA_BROWSER_UI_VIEWS_FRAME_CONTENT_FRAME_H_

#include "ui/gfx/geometry/insets.h"

namespace strata {

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
// edge.
inline constexpr gfx::Insets GetContentFrameInsets() {
  return gfx::Insets::TLBR(kContentFrameInset, 0, kContentFrameInset,
                           kContentFrameInset);
}

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_FRAME_CONTENT_FRAME_H_
