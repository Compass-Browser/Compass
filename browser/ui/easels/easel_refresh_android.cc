// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/easels/easel_refresh.h"

#include <utility>

namespace compass::easels {

// A page renders on Android only in a view attached to a window
// (WindowAndroid and its compositor), and Chrome's hidden tabs don't draw;
// a page no one sees would need a window of its own. Until then, Refresh
// is the desktop's.
void RefreshCapture(Profile* profile,
                    const std::string& object_uuid,
                    base::OnceCallback<void(std::string error)> done) {
  std::move(done).Run("Refresh isn't available on Android yet.");
}

}  // namespace compass::easels
