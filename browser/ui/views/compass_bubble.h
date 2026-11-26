// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_COMPASS_BUBBLE_H_
#define COMPASS_BROWSER_UI_VIEWS_COMPASS_BUBBLE_H_

#include <memory>

#include "base/memory/weak_ptr.h"

class BrowserWindowInterface;
class WebUIBubbleDialogView;
class WebUIContentsWrapper;

namespace content {
class WebContents;
}  // namespace content

namespace gfx {
class Rect;
}  // namespace gfx

namespace compass {

// Shows |contents| (a Compass page whose controller is a
// TopChromeWebUIController) in a bubble of |browser|'s window beside the
// sidebar, pointing at |anchor| (in screen coordinates): a row or button of
// the sidebar. The bubble owns the page, takes the window's space's tint on
// its frame (BubbleLook: the page sets the same colors on itself), and shows
// once the page calls its embedder's ShowUI(). It opens upwards from an
// anchor in the window's lower half (the space switcher), downwards
// otherwise.
base::WeakPtr<WebUIBubbleDialogView> ShowCompassBubble(
    BrowserWindowInterface* browser,
    std::unique_ptr<WebUIContentsWrapper> contents,
    const gfx::Rect& anchor);

// Closes |bubble| at once, if it's open.
void CloseCompassBubble(base::WeakPtr<WebUIBubbleDialogView> bubble);

// |bubble|'s page, or null if it's gone.
content::WebContents* GetCompassBubbleContents(
    base::WeakPtr<WebUIBubbleDialogView> bubble);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_COMPASS_BUBBLE_H_
