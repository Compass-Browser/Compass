// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/start_updater.h"

#include "build/build_config.h"
#include "compass/browser/updates/buildflags.h"

#if BUILDFLAG(IS_MAC)
#include "compass/browser/updates/sparkle_updater_mac.h"
#endif
#if BUILDFLAG(IS_WIN) && BUILDFLAG(ENABLE_WIN_UPDATER)
#include "compass/browser/updates/win_updater.h"
#endif

namespace compass::updates {

void StartUpdater() {
#if BUILDFLAG(IS_MAC)
  StartSparkleUpdater();
#elif BUILDFLAG(IS_WIN) && BUILDFLAG(ENABLE_WIN_UPDATER)
  StartWinUpdater();
#endif
}

}  // namespace compass::updates
