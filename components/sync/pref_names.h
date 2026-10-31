// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_PREF_NAMES_H_
#define COMPASS_COMPONENTS_SYNC_PREF_NAMES_H_

// Profile prefs of Compass sync, registered by
// compass::sync::CompassSyncServiceFactory.
namespace compass::sync::prefs {

// The sync server, e.g. "https://sync.example.com/v2" (go-sync's prefix: the
// engine posts to <url>/command/, pairing goes to <url>/compass/pair).
// Empty: none set. --sync-url wins over it.
inline constexpr char kServerUrl[] = "compass.sync.server_url";

// The chain's seed, encrypted with os_crypt_async, base64. Empty: this
// profile isn't in a chain.
inline constexpr char kSeed[] = "compass.sync.seed";

// The chrome://compass-sync switches turned off, by name
// (compass/browser/sync/sync_settings.h). Kept across leaving and joining.
inline constexpr char kDisabledTypes[] = "compass.sync.disabled_types";

// "Reset sync" replaced the chain: until this device has put the new chain's
// passphrase in place, a passphrase it finds already set (its old chain's,
// cached) doesn't release the data types.
inline constexpr char kResetPending[] = "compass.sync.reset_pending";

}  // namespace compass::sync::prefs

#endif  // COMPASS_COMPONENTS_SYNC_PREF_NAMES_H_
