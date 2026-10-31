// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/controllers_hook.h"

#include <utility>

#include "base/no_destructor.h"

namespace compass::sync {

namespace {

ExtraControllersFactory& Factory() {
  static base::NoDestructor<ExtraControllersFactory> factory;
  return *factory;
}

}  // namespace

void SetExtraControllersFactory(ExtraControllersFactory factory) {
  Factory() = std::move(factory);
}

syncer::DataTypeController::TypeVector CreateExtraControllers(
    content::BrowserContext* context,
    syncer::SyncService* sync_service) {
  if (!Factory()) {
    return {};
  }
  return Factory().Run(context, sync_service);
}

}  // namespace compass::sync
