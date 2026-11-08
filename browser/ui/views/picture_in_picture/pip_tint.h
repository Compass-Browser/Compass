// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_TINT_H_
#define COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_TINT_H_

#include <optional>

#include "base/functional/callback.h"
#include "third_party/skia/include/core/SkColor.h"

namespace content {
class WebContents;
}

namespace compass {

// The color of the space |contents| (a tab) belongs to, which seeds its
// picture-in-picture window's colors (PipStyle), or nullopt for none: grey,
// off the record, an extension theme, or no tab. The sidebar, which knows
// the spaces, provides it (InstallPipTint(), sidebar_tint.cc); PipStyle,
// in upstream's overlay target, asks for it.
using PipTintProvider =
    base::RepeatingCallback<std::optional<SkColor>(content::WebContents*)>;
void SetPipTintProvider(PipTintProvider provider);
std::optional<SkColor> GetPipTint(content::WebContents* contents);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_PICTURE_IN_PICTURE_PIP_TINT_H_
