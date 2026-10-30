// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_SWAP_H_
#define STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_SWAP_H_

#include "base/time/time.h"

namespace strata {

// Makes a switch to another profile's space (SwitchWindowToSpace(),
// window_features.h) happen in the same native window: once the new
// profile's window, built hidden, has its sidebar tree, its BrowserView takes
// the shown native window (BrowserViewExchange), which keeps its last frame
// until the sidebar and the page have painted (at most kMaxSwapWait). See
// docs/design/profiles-in-place.md. Idempotent.
void InstallSwapPresenter();

// How long an exchange waits for the new sidebar's tree (the debug build is
// slower than kMaxTreeWait allows).
void SetMaxTreeWaitForTesting(base::TimeDelta wait);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_SWAP_H_
