// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_CONTROLLERS_HOOK_H_
#define COMPASS_COMPONENTS_SYNC_CONTROLLERS_HOOK_H_

#include "base/functional/callback.h"
#include "components/sync/service/data_type_controller.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace syncer {
class SyncService;
}  // namespace syncer

// How Compass's own data types (COMPASS_SPACES) reach the sync service.
// //chrome/browser/sync builds the controllers and can't depend on the
// browser-side Compass code that makes ours (that code depends on it), so the
// browser side registers a factory here at startup and
// chrome/browser/sync/sync_service_factory.cc calls it (see
// patches/chrome-browser-sync-sync_service_factory.cc.patch).
namespace compass::sync {

using ExtraControllersFactory =
    base::RepeatingCallback<syncer::DataTypeController::TypeVector(
        content::BrowserContext* context,
        syncer::SyncService* sync_service)>;

// Set once, before profiles load (CompassSyncServiceFactory does it).
void SetExtraControllersFactory(ExtraControllersFactory factory);

// The controllers of Compass's data types for |context|'s sync service; none
// if no factory is set (unit tests).
syncer::DataTypeController::TypeVector CreateExtraControllers(
    content::BrowserContext* context,
    syncer::SyncService* sync_service);

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_CONTROLLERS_HOOK_H_
