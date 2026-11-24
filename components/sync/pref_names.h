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

// The server said this device was removed from the chain on another device
// (docs/design/sync.md, "Removing a device"): until the user leaves, the
// engine sends nothing and the sync page asks to leave. Cleared on leaving.
inline constexpr char kDeviceRemoved[] = "compass.sync.device_removed";

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

// Profile account: the server (its URL) this account was linked on, from the
// chain's own (POST /compass/account/link), so a private server lets it in
// (docs/design/sync.md, "Profiles"). Empty: not linked yet.
inline constexpr char kAccountLinkedServer[] =
    "compass.sync.account_linked_server";

// Company sign-in (docs/design/sync.md, "Company sign-in (OIDC)").
//
// Home profile: sync UUIDs of the chain's profile accounts the server may
// not have linked to the chain's sign-in yet (POST
// /compass/account/link failed, or hasn't run). Tried again at each start
// and after signing in.
inline constexpr char kLoginLinksPending[] = "compass.sync.login_links_pending";

// Home profile: the server Compass set up in the user's Cloudflare account
// (docs/design/sync.md, "A free server on Cloudflare"), a dictionary with
// "account_id", "script" and "subdomain". Empty: the server wasn't set up
// by Compass. The Cloudflare token itself is never stored.
inline constexpr char kCloudflare[] = "compass.sync.cloudflare";

}  // namespace compass::sync::prefs

#endif  // COMPASS_COMPONENTS_SYNC_PREF_NAMES_H_
