// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_EASELS_EASEL_REFRESH_H_
#define COMPASS_BROWSER_UI_EASELS_EASEL_REFRESH_H_

#include <string>

#include "base/functional/callback.h"

class Profile;

namespace compass::easels {

// Refresh (docs/design/easels.md, "Captures"): takes the capture
// |object_uuid| again. Its URL loads in a page no window shows, with the
// profile's cookies, at the saved viewport size; once it has loaded and
// 1.5 s more have passed, it's scrolled to the saved offset and the same
// region is copied. The new image replaces the old one (image_asset and
// capture_time). |done| gets "" when it worked, else why not. Desktop only:
// on Android it says so.
void RefreshCapture(Profile* profile,
                    const std::string& object_uuid,
                    base::OnceCallback<void(std::string error)> done);

}  // namespace compass::easels

#endif  // COMPASS_BROWSER_UI_EASELS_EASEL_REFRESH_H_
