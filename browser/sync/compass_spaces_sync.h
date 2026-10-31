// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_COMPASS_SPACES_SYNC_H_
#define COMPASS_BROWSER_SYNC_COMPASS_SPACES_SYNC_H_

namespace compass::sync {

// Makes every sync service built from now on run COMPASS_SPACES: registers
// its controller with compass/components/sync/controllers_hook.h. Called once,
// before profiles load (CompassSyncServiceFactory's constructor).
//
// The controller runs the type only while the profile's CompassSyncService
// says the chain's passphrase is in place (AreDataTypesReleased()); it isn't
// a user-selectable type. Its bridge (CompassSpacesSyncBridge) is created
// the first time sync asks for it, and lives with the profile's
// SpaceService.
void RegisterSpacesSyncController();

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_COMPASS_SPACES_SYNC_H_
