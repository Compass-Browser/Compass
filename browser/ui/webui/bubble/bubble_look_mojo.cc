// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/bubble/bubble_look_mojo.h"

#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "compass/browser/ui/views/bubble_look.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/color_utils.h"

namespace compass {

bubble::mojom::LookPtr GetBubbleLookFor(content::WebContents* contents) {
  // The window's: the bubble's (ShowCompassBubble() sets it), or the tab's.
  BrowserWindowInterface* browser = webui::GetBrowserWindowInterface(contents);
  if (!browser) {
    if (tabs::TabInterface* tab =
            tabs::TabInterface::MaybeGetFromContents(contents)) {
      browser = tab->GetBrowserWindowInterface();
    }
  }
  const BubbleLook look = ComputeBubbleLook(&contents->GetColorProvider(),
                                            GetBubbleTint(browser));
  auto result = bubble::mojom::Look::New();
  result->background = color_utils::SkColorToRgbaString(look.background);
  result->card = color_utils::SkColorToRgbaString(look.card);
  result->pill = color_utils::SkColorToRgbaString(look.pill);
  result->track = color_utils::SkColorToRgbaString(look.track);
  result->hairline = color_utils::SkColorToRgbaString(look.hairline);
  result->text = color_utils::SkColorToRgbaString(look.text);
  result->text_dim = color_utils::SkColorToRgbaString(look.text_dim);
  result->accent = color_utils::SkColorToRgbaString(look.accent);
  result->dark = look.dark;
  return result;
}

}  // namespace compass
