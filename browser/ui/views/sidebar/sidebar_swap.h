// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_SWAP_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_SWAP_H_

#include "base/time/time.h"

namespace strata {

// Makes window swaps (SwitchWindowToSpace(), window_features.h) look like one
// window changing space: the new window goes up transparent over the old one,
// without animations, and turns opaque in the same moment the old one hides,
// once its sidebar and page have painted and that frame is on screen (at most
// kMaxSwapWait later). See docs/design/spaces.md, "Swapping the window".
// Idempotent.
void InstallSwapPresenter();

// How long an exchange waits for the new sidebar's tree (the debug build is
// slower than kMaxTreeWait allows).
void SetMaxTreeWaitForTesting(base::TimeDelta wait);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_SWAP_H_
