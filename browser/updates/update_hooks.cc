// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/updates/update_hooks.h"

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "compass/browser/updates/buildflags.h"

namespace compass::updates {
namespace {

Updater* g_updater = nullptr;

struct Pending {
  base::Lock lock;
  std::optional<base::Version> version;
};

Pending& GetPending() {
  static base::NoDestructor<Pending> pending;
  return *pending;
}

}  // namespace

Updater* GetUpdater() {
  return g_updater;
}

void SetUpdater(Updater* updater) {
  g_updater = updater;
}

std::optional<base::Version> PendingVersion() {
  Pending& pending = GetPending();
  base::AutoLock hold(pending.lock);
  return pending.version;
}

void SetPendingVersion(const base::Version& version) {
  Pending& pending = GetPending();
  base::AutoLock hold(pending.lock);
  pending.version = version;
}

std::string CompassVersion() {
  // compass_version in GN: `compass release` sets it. On macOS the app's
  // Info.plist carries it too (CFBundleVersion, for Sparkle).
  return BUILDFLAG(COMPASS_VERSION);
}

}  // namespace compass::updates
