// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UPDATES_WIN_UPDATER_H_
#define COMPASS_BROWSER_UPDATES_WIN_UPDATER_H_

namespace compass::updates {

// Starts the Windows updater in release builds (compass_version and the
// public key set); dev builds don't update. UI thread, once Local State and
// the network service are up.
void StartWinUpdater();

}  // namespace compass::updates

#endif  // COMPASS_BROWSER_UPDATES_WIN_UPDATER_H_
