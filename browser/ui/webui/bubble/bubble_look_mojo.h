// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_BUBBLE_BUBBLE_LOOK_MOJO_H_
#define COMPASS_BROWSER_UI_WEBUI_BUBBLE_BUBBLE_LOOK_MOJO_H_

#include "compass/browser/ui/webui/bubble/bubble.mojom.h"

namespace content {
class WebContents;
}  // namespace content

namespace compass {

// The look of the bubble, or tab, |contents| shows in: its window's space's
// tint (compass::BubbleLook).
bubble::mojom::LookPtr GetBubbleLookFor(content::WebContents* contents);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_BUBBLE_BUBBLE_LOOK_MOJO_H_
