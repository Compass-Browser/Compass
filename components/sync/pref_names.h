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

// Profiles (docs/design/sync.md, "Profiles").
//
// The profile's sync UUID, lowercase: made once, kept for the profile's life
// unless joining maps the profile to another device's (it then takes that
// one's). A profile account derives its keys from it.
inline constexpr char kProfileUuid[] = "compass.sync.profile_uuid";

// True: the profile syncs with its own account, HKDF(seed,
// "compass-sync-auth:" + uuid), and passphrase. False (the default, and every
// chain of slices 2-4): the chain's own account; with a seed, that's the home
// profile, which also carries the chain's profile list.
inline constexpr char kProfileAccount[] = "compass.sync.profile_account";

// Home profile: joined by recovery code while other profiles of this device
// can sync, so they still wait to be mapped to the chain's (the list comes
// with the first sync).
inline constexpr char kProfilesPending[] = "compass.sync.profiles_pending";

// Home profile: this device's profiles in the chain, as {sync UUID: profile
// directory name}, so one that isn't loaded (no space in the switcher) isn't
// created again.
inline constexpr char kProfileDirs[] = "compass.sync.profile_dirs";

// Home profile: sync UUIDs of the chain's profiles deleted on this device,
// which it doesn't create again.
inline constexpr char kProfilesRemoved[] = "compass.sync.profiles_removed";

}  // namespace compass::sync::prefs

#endif  // COMPASS_COMPONENTS_SYNC_PREF_NAMES_H_
