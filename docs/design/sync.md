# Sync between devices

Based on Chromium 157.0.8103.51.

ARCHITECTURE.md §1 already commits to "our own end-to-end encrypted,
self-hostable service" (Chrome Sync is closed to third-party builds since
2021). This doc says how.

## Goal

A second Mac (and later Windows/Linux) shows the same spaces, pinned
entries, folders and favorites, and has the same passwords, history,
extensions and settings, without a Google account and without the server
being able to read any of it.

## The idea in one paragraph

Keep Chromium's sync engine (`components/sync`) and point it at a server
of our own that speaks the same protocol. Chromium's existing data types
(passwords, history, autofill, extensions, settings, open tabs, device
list) then sync with little new code. Compass's sidebar becomes one more
data type, `COMPASS_SPACES`. There are no accounts: as in Brave's "sync
chain", the first device makes a random seed; other devices get it by
typing a short pairing code shown on a device already syncing (or a
12-word recovery code). Every device with the seed derives the same server
identity (a signing key) and the same encryption passphrase (Chromium's
custom passphrase, so everything is encrypted on the device). The server
stores opaque blobs.

## Options considered

| Option | Verdict |
|---|---|
| **Chromium sync engine + own server (chosen)** | All Chromium types for free, proven conflict handling and E2E encryption (Nigori custom passphrase), sync-internals for debugging. Costs: a server to run, a set of small upstream patches, an auth override |
| CloudKit | No server, Apple ID auth. But macOS only (§1 wants Windows and Linux), needs the paid Apple Developer team signing and entitlements (we sign with a self-made identity), and covers only what we write ourselves: passwords/history would need a second mechanism |
| Our own JSON sync of `Compass Sidebar.json` (server or iCloud Drive folder) | Small to write, but only the sidebar, and file-level last-writer-wins loses edits made on two devices; iCloud Drive file conflicts are worse |
| Chromium's local sync backend file in iCloud Drive | `LoopbackServer` writes one file and assumes one writer; two Macs would overwrite each other |
| Map the sidebar onto `BOOKMARKS` | No new type, but splits, favorites, space colors and profiles don't fit, and bookmarks UI would show the tree |

## What syncs

Compass data (`COMPASS_SPACES`):

- Spaces: name, color and theme, icon, order, auto-archive delay, profile.
- Pinned trees: folders (nested), tabs (URL, title, custom title), splits.
- Favorites.
- Today (unlike Arc): entries, folders and splits, with their last-active
  times. See "Today across devices".
- The archive (entries, archived-at times), capped to the last 30 days.
- Folder collapse state.
- Favicons of entries' hosts, as a device got them from a loaded page.
- The chain's list of profiles.
- Easels: boards, the objects on them and their images (easels.md).
- Not synced, because it can't be or is per window: live state (which node
  has a tab, which window shows what), sidebar width and collapse, the
  selected entry.

Chromium types, on by default: `PASSWORDS`, `HISTORY` (with its delete
directives; see "History"), `AUTOFILL` (and addresses), `EXTENSIONS`,
`EXTENSION_SETTINGS`, `PREFERENCES` (only syncable prefs), `SESSIONS`
(open tabs per device), `DEVICE_INFO` (device list), `SEARCH_ENGINES`,
`READING_LIST`, `BOOKMARKS`, `NIGORI`, `SEND_TAB_TO_SELF` ("Send to Your
Devices"; new entries reach Macs through change notifications and iPhones
through APNs, send-tab.md).

Off (they need Google back ends or don't apply): the `AUTOFILL_WALLET_*`
types, `PLUS_ADDRESS*`, `SHARING_MESSAGE`, `SECURITY_EVENTS`,
`USER_CONSENTS`, `WEBAUTHN_CREDENTIAL` (needs Google's security domain
secrets), `SAVED_TAB_GROUP` / `SHARED_TAB_GROUP_*` / collaboration types
(our folders replace groups), `POWER_BOOKMARK`, `PRODUCT_COMPARISON`,
`THEMES` (space colors theme the window), `APPS`, incoming password
sharing. How they're turned off: "Types" below.

## Identity: a sync chain, no accounts

No accounts by default. A server can require company sign-in (OIDC) before
a chain may sync (see "Company sign-in (OIDC)"); that gates access only,
and the chain, its keys and the encryption below stay as they are.

- **Seed**: 16 random bytes (128 bits, the strength of the AES keys that
  encrypt the data), made by the first device.
- **Recovery code**: the seed as 12 BIP39 words, shown once when sync
  starts so the user can save it (a password manager, paper), and again
  from settings behind Touch ID. Only needed when no synced device is left:
  lose every device and the code, and the synced data is gone. The UI says
  so. (24 words was too long.)
- **Adding a device** uses a pairing code, not the words (see "Pairing").
- From the seed, with HKDF-SHA256 (BoringSSL):
  - `info="compass-sync-auth"` → an Ed25519 key pair. The public key is
    the chain's ID on the server.
  - `info="compass-sync-passphrase"` → Chromium's custom passphrase
    (base64). Set with `SyncUserSettings::SetEncryptionPassphrase()` or
    `SetDecryptionPassphrase()` (see "Passphrase"). All types encrypted;
    the server sees types, IDs, versions, sizes, times.
- **Auth to the server**: go-sync's own token format, kept as is (no
  server change): per request,
  `base64url_padded(HEX(ts_ms) | HEX(ed25519_sign(ts_ms)) | HEX(pubkey))`,
  where `ts_ms` is the ASCII decimal Unix time in ms, sent as
  `Authorization: Bearer <token>`. The server checks the signature and a
  ±24 h window (go-sync's `auth/auth.go`); the chain ID is the hex
  public key. No passwords, no email.
- **Derivations** (fixed): seed = 16 bytes;
  `HKDF-SHA256(ikm=seed, salt="", info="compass-sync-auth", L=32)` = the
  Ed25519 private seed; `HKDF(..., info="compass-sync-passphrase", L=32)`,
  base64 (standard, padded) = the custom passphrase. Profile accounts
  append `":" + profile_uuid` to both infos: each has its own account and
  its own passphrase (see "Profiles").
  Test vectors (`components/sync/chain_unittest.cc`, `bip39_unittest.cc`;
  computed independently with Go's `crypto/hkdf` and `crypto/ed25519`):

  | | |
  |---|---|
  | seed | `000102030405060708090a0b0c0d0e0f` |
  | recovery code | `abandon amount liar amount expire adjust cage candy arch gather drum buyer` |
  | auth key (Ed25519 seed) | `be6eabbf861e7b696f17ae6bd46b4bd745d1190e0658e720ca55fa65104139ad` |
  | public key = chain ID | `696a94d1a2ebbabf8e5012c07df3c65c7c6beaaf8ccaf4a67e6cdd3da9743467` |
  | passphrase | `xoFg1Bm10gjM8Ahji5ibCeSkV6zo7kZEcjBFP9M8s50=` |
  | auth key, profile `2b5c3e1a-0000-4000-8000-000000000001` | `1b28e57a6e3ca95bc1b4b9c30f10f8d554ef09eed00844b52048aea6d90f4cf4` |
  | its public key = account ID | `f1fe31e2c21e376e8d3b2ef57560b8466b37486e5fa46cc60bb39f1ea2ec9fab` |
  | its passphrase | `C89l8oqXaTa997+mU79acx+BGgUNf6YPtMoPU9ODyEc=` |
  | token at ts 1700000000000 | `MzEzNzMwMzAzMDMwMzAzMDMwMzAzMDMwMzB8MkY1REY4REVCNDUwRUJBMTk5MTgxNjdGODZFQjVDOThBRkQxREYyRUI5MTUzQjBDOENGMDQ1RDhGQjZDODk3MERFMjhFOUI0Qzg0QzJCMjUzRDcxMzI4QTVCNUJENzcwNUE0QTQ1QjFCQkVERkJBRjM5MzcyQTA1RTZEOTZCMDV8Njk2QTk0RDFBMkVCQkFCRjhFNTAxMkMwN0RGM0M2NUM3QzZCRUFBRjhDQ0FGNEE2N0U2Q0REM0RBOTc0MzQ2Nw==` |

  The recovery code is BIP39 (English list, 128-bit entropy + 4-bit
  SHA-256 checksum); the reference vectors (e.g. `7f7f…7f` → `legal winner
  thank year wave sausage worth useful legal winner thank yellow`) pass.
- **Stored on the device**: the seed, encrypted with `os_crypt_async` (the
  same Keychain key as passwords), in a profile pref (`compass.sync.seed`).
- **Removing a device** deletes its `DEVICE_INFO` entry for every device
  and the server refuses that installation from then on:
  `DELETE /v2/compass/devices/{cache_guid}?client_id=<caller's cache GUID>`
  (sync token, gated as `/command/`; the GUID percent-encoded, Chromium's
  are base64) answers 204 (also when already removed, or when the device
  had no entry), 400 `{"error":"bad_request"}` (no or a too long GUID or
  `client_id`, over 128), 409 `{"error":"this_device"}` (the caller's own
  GUID), 403 `{"error":"chain_disabled"}`. The server tombstones the entity
  with client tag `DeviceInfo_<cache GUID>` through the commit path
  (`command.DeleteDeviceInfo`: new version and mtime, tag freed, item count
  and bytes down, the type's cached mtime moved), so the others get the
  tombstone at their next GetUpdates (they're notified on
  `/compass/events`); records the GUID in `removed_devices`; deletes its
  push tokens; and ends its change stream. From then on any request whose
  `client_id` query parameter is that GUID (`/command/`, `/compass/events`,
  `/compass/push`, `/compass/quota`, `/compass/devices`) gets 403
  `{"error":"device_removed"}` with `X-Compass-Device: removed`
  (`auth.RejectRemovedDevices`); requests without a `client_id` and pairing
  are unaffected. It still can't revoke the device, because every device
  holds the seed: the removed device keeps its local data and can join
  again (a rejoin is a new cache GUID). Admin: `devices list|remove`.
  "Reset sync" makes a new chain (new seed), uploads this device's data,
  and the others must join again. Same as Brave.

### Pairing

A device already syncing (A) passes the seed to a new one (B) through the
server, protected by a short code. The server only relays messages it
can't read, and a wrong guess burns the code.

1. On A, "Add a device": A opens a pairing slot on the server (signed
   request, as for sync). The server gives the lowest free slot number;
   the slot lives 5 minutes. A shows the code `<slot>-<6 digits>`, e.g.
   `7-482913`, and a QR code of the invite
   `compass-pair:?server=<escaped server URL>&code=<code>` that a phone's
   camera opens in Compass for iOS ("Pairing QR code" below).
   The slot number only finds the pairing; the 6 digits are the secret and
   never reach the server.
2. On B, "I have another device": B enters the server URL (self-hosted:
   it isn't known yet) and the code.
3. A and B run **SPAKE2** (BoringSSL's `SPAKE2_CTX`, A as Alice, B as Bob,
   password = the 6 digits, names bound to the slot and server host),
   relaying their messages through the slot. Both end with key K, then
   exchange confirmation MACs. A mismatch (wrong code, or someone in the
   middle) closes the slot; A offers a new code. One guess per code, so 6
   digits (1 in a million) is enough.
4. B sends its device name encrypted with K; A asks “Add MacBook?”
   (Allow/Deny), which also catches someone who glimpsed the code.
5. On Allow, A sends AES-GCM(K, seed, chain's profile list); B derives
   everything from the seed and starts syncing (first sync merges).

"Use a recovery code" on B is the fallback: type the 12 words (BIP39
autocomplete, checksum checked as you type).

#### Pairing API

Under go-sync's prefix, next to `/v2/command/` (`server/pairing`). JSON
bodies, UTF-8. Binary fields are standard base64.

- `POST /v2/compass/pair` with `Authorization` (A's sync token) →
  `201 {"slot": 7, "expires_in": 300, "server": "sync.example.com"}`. The
  lowest slot ≥ 1 that is neither open nor closed in the last 300 s.
  Opening a new slot closes the chain's previous one (one open slot per
  chain; reason `cancel`). `server` is the name both devices bind into
  SPAKE2: `COMPASS_SYNC_PUBLIC_HOST`, else the `Host` of this request,
  without its port, lower-case. Two devices that reach the server by
  different names (IP vs hostname, a Tailscale name) would otherwise
  compute different keys and fail as "wrong code".
- `POST /v2/compass/pair/{slot}/join` (no auth; B) →
  `200 {"b_token": "<random 128-bit, hex>", "server": "...",
  "expires_in": <s left, rounded up>}`. Only the first join succeeds;
  after it, `409`. Unknown slot: `404`; closed or expired: `410` (below).
  Rate limit: 10 joins per IP per 10 min, then `429`.
- `POST /v2/compass/pair/{slot}/msg` with body
  `{"from": "a"|"b", "data": "<base64, ≤ 4 KiB>"}`. From A: the
  `Authorization` of the chain that opened it. From B: header
  `X-Compass-Pair-Token: <b_token>`. → `200 {"seq": n}`. At most 8
  messages per side, then `429`.
- `GET /v2/compass/pair/{slot}/msg?for=a|b&after=<seq>` (same auth as
  above, for the reader) → long poll up to 25 s: `200 {"messages":
  [{"seq": n, "data": "..."}]}` (possibly empty on timeout). Sequence
  numbers count per sender, from 1.
- `DELETE /v2/compass/pair/{slot}?reason=mismatch|cancel|deny` (A's auth,
  or B's token) → `204`. It closes the slot; no reason is a `cancel`
  (older clients), another value `400`.
- A closed slot says why: for 300 s after it closes (`Config.ClosedTTL`),
  every request on its number answers `410 {"reason": "mismatch" |
  "cancel" | "deny" | "expired"}` (`expired` when its 300 s ran out), and
  the number isn't given out again meanwhile, so a code typed late reaches
  the reason, not someone else's new slot; then `404`. Long polls waiting
  on a slot that closes get the same `410`.
- Other errors: `403` for another chain or a wrong `b_token`; `413`/`400`
  for bad data; `429`/`503` on open = try later.
- Slots live 300 s from opening, in memory only (a restart drops them).

Messages (client-side, the server doesn't read them):
1. B → A: SPAKE2 message (Bob).
2. A → B: SPAKE2 message (Alice).
3. B → A: `HMAC-SHA256(K_confirm_b, "compass-pair-confirm-b")`, then
   `AES-256-GCM(K_enc, device name)`; K = SPAKE2 key, split with HKDF
   (`info="compass-pair-enc"`, `"compass-pair-confirm-a"`,
   `"compass-pair-confirm-b"`). SPAKE2 names: `"compass-a:" + host + ":" +
   slot` / `"compass-b:" + host + ":" + slot`.
4. A → B: A's confirmation MAC, then either `AES-256-GCM(K_enc, {"seed":
   ..., "profiles": [...]})` or a denial. A wrong MAC on either side →
   `DELETE` and a new code.

Framing (`components/sync/pairing.h`); one relay message per step:

- 1 and 2: the raw 32-byte SPAKE2 message (BoringSSL's SPAKE2 over
  edwards25519; its key is 64 bytes, all of it the HKDF input, salt empty,
  32 bytes per subkey).
- 3: `MAC_b (32) || nonce (12) || AES-256-GCM(K_enc, name, ad="compass-pair-b")`.
- 4: `MAC_a (32) || verdict (1 byte: 0x01 allow, 0x00 deny)`, then on allow
  `nonce (12) || AES-256-GCM(K_enc, {"seed": base64(16 bytes), "profiles": [...]}, ad="compass-pair-a")`.
  `"profiles"` is the chain's list, `[{"uuid": "<lowercase UUID>", "name":
  "Work", "avatar": 26, "home": true}, ...]` (`avatar` only if set, `home`
  only on the home profile); a joiner ignores entries it can't read, and
  treats an empty list as unknown.
- `host` in the SPAKE2 names is the `server` of the open (A) and join (B)
  replies; with an older server that doesn't send it, `GURL::host()` of
  the server URL each side has (no port, lowercase).
- A doesn't close the slot after sending an allow (B must still read it);
  B closes it once it has 4. After a deny A closes it with `deny` (B reads
  the answer or the `410`, either way "Not added"). A closes it with
  `mismatch` on a wrong MAC, `cancel` on cancel or expiry; B the same way.
- B maps the 410s to "Wrong code", "The other device canceled", "Not
  added", "Code expired" (`kWrongCode`, `kCanceled`, `kDenied`,
  `kExpired`; a bare 404 from an older server is `kClosed`). B's timeout
  is the join's `expires_in` + 30 s (5.5 min with an older server).
- Vectors for K = `00 01 … 3f`: K_enc `5144144d…7a857653`, K_confirm_a
  `d71f8461…63ef6a2a`, K_confirm_b `46d4fef6…dd61a05a`, MAC_a
  `5640ef87…7f3366f8`, MAC_b `d0ccb754…12ff0ac4` (full values in
  `pairing_unittest.cc`, `PairingKeysTest`).

Compatibility: old clients keep working with a newer server (a DELETE
without reason is a cancel, unknown JSON fields are ignored, a 410 where
they expected 404 only changes their message).

#### Pairing QR code

While A shows the code (`kShowingCode`), the page handler also sends the
invite and its QR (`Pairing.invite_url`, `qr`, `qr_size` in
`compass_sync.mojom`): `CompassSyncPageHandler::PairingInviteUrl()` builds
`compass-pair:?server=` + `base::EscapeQueryParamValue(server URL)` +
`&code=` + the code, and `//components/qr_code_generator` (upstream's,
Rust-backed) makes the modules. The page draws them as SVG, black on white
with a four-module quiet zone whatever the theme. The iOS app's
`PairingInvite(url:)` reads the escaped form (`PairingCryptoTests.
parsesTheDesktopsEscapedInvite`). `tools/sync_e2e/sync_status.js` reports
the invite as `qr`, and `decode_qr.swift` reads a screenshot back.

Under the code, "Expires in m:ss" counts down to the slot's end
(`PairingStatus.expires_at` = when the code showed + the server's
`expires_in`; `Pairing.expires_at_ms` on the page), red in the last 30 s.
At zero the host's own timer ends the pairing with "The code expired. Try
again with a new code."; the page only displays the time.

## Plugging it into Chromium

The engine wants a signed-in account (`SyncAuthManager` gets an OAuth token
from `IdentityManager`). Two ways were considered:

1. **Synthesized account**: `SyncAuthManager` returns a `SyncAccountInfo`
   with `gaia = "compass:" + hex(pubkey[0..16])` and our signed token from
   `RequestAccessToken()`, and Compass sets that account as primary in
   `IdentityManager` without Gaia. Brave does this. Not built: it needs
   `AccountsMutator`/`PrimaryAccountMutator`, a quiet account reconciler
   and other Gaia services, and a `RequestAccessToken()` patch.
2. **Local-sync path (chosen)**: `kEnableLocalSyncBackend` already runs the
   engine with no identity at all, but `EngineComponentsFactoryImpl` then
   builds a `LoopbackConnectionManager`. A hook in
   `SyncServiceImpl::TryStartImpl()` swaps in our own engine components
   factory, whose connection manager posts to the server and signs each
   request. Local sync limits types (`LocalSyncSupportedTypes()`); History
   needs extra work ("History").

What route 2 gives:

- **Mode**: sync-the-feature stays off (`sync_consent=0`,
  `IsSyncFeatureEnabled()` false, first-time setup not complete), yet the
  data types run as full sync. Synced passwords land in the profile store
  (`storedIn: DEVICE`), not an account store, which is what we want.
- **The passphrase survives a restart**: a device comes back with
  `kCustomPassphrase` and nothing required. Sync's own Nigori storage keeps
  the keys, so the missing Gaia-keyed bootstrap token doesn't matter. A
  wrong passphrase gives a clean "Needs passphrase" state (cryptographer
  pending keys, data types stopped, nothing decrypted).
- **No Google traffic from sync**: `clients4.google.com/chrome-sync` and
  `oauth2.googleapis.com` never appear. Every Google host seen with sync on
  (`update.googleapis.com`, `edgedl.me.gvt1.com`, `www.google.com`,
  `clients2`, `accounts`, `safebrowsing`) also shows with `--disable-sync`.
  That baseline traffic (component updater, Safe Browsing, DNS-over-HTTPS)
  is a separate privacy task, not sync's.
- **go-sync and Chromium 156**: no protocol errors, including types newer
  than go-sync's protos (WebAuthn, Saved Tab Group). The server stores
  types it doesn't know by field number.

The server URL comes from pref `compass.sync.server_url` (hook in
`GetSyncServiceURL()`; `--sync-url` keeps working and wins over the pref).
Google sign-in UI and sync promos stay hidden.

### Code layout

- `//compass/components/sync:chain`: seed, BIP39 recovery code,
  derivations, tokens (`chain.h`, `bip39.h`), pairing on both sides with a
  transport interface (`pairing.h`) and its HTTP implementation
  (`pairing_network_transport.h`), profile lists and mapping
  (`profile_list.h`).
- `//compass/components/sync:engine`: the engine components
  (`CompassEngineComponentsFactory`, a connection manager that signs each
  request), `SyncCredentials` (the chain key shared between the browser
  side and the sync sequence; the first request waits up to 20 s for the
  seed to decrypt) and `sync_hooks.h`, what `SyncServiceImpl` calls.
  `//components/sync/service` depends on this target; nothing in it needs
  `SyncService`, so there's no dependency cycle and the patch is one
  `deps` line.
- `//compass/components/sync:notifier`: change notifications and the quota
  check (`compass_sync_notifier.{h,cc}`, `compass_sync_quota.{h,cc}`).
- `//compass/browser/sync`: `CompassSyncService` (keyed service, created
  with each regular profile): the seed and server prefs, passphrase, type
  selection, refresh on focus (`ProfileBrowserCollection`) and wake
  (`PowerSuspendObserver`, at most every 10 s), pairing, profiles.
  `SyncController` is its interface for the page. Also the
  `COMPASS_SPACES` bridge.
- `chrome://compass-sync` (`browser/ui/webui/sync/`,
  `resources/sidebar/compass_sync.html`), also shown as the Sync section of
  compass://settings (`resources/sidebar/settings/sync_page.{html,ts}`).

### When sync runs

The engine talks to the chain's server when the profile has
`kEnableLocalSyncBackend` (route 2), a seed and a server URL.
`SyncServiceImpl` reads `kEnableLocalSyncBackend` once, when the profile
loads, so "Start sync", joining, "Leave sync" and "Reset sync" take effect
after a restart; the page says so and has a "Restart Compass" button.
Turning sync on also sets `kLocalSyncBackendDir` (to `<profile>/Compass
Sync`, never written): on macOS `SyncServiceFactory` doesn't create a
local-sync service without it. Leaving clears the seed and the local-sync
pref; the next launch's sync service, not signed in, clears sync's
metadata as for a signed-out user and keeps the data.

While it starts, the page names the step it waits on
(`StartingStatus()`, `CompassSyncService::GetStartingStep()`): reading the
sync key, connecting to the server (the engine isn't up), setting up
encryption (types held for the passphrase), or getting the data ready
(configuring). One step that takes 60 s (`kStartingStuckAfter`, a timer
wakes the page) adds "This is taking long. Restart Compass if it doesn't
change." and the Restart button (`SyncStatus::offer_restart`).

### Passphrase

At each start with a chain, `CompassSyncService` selects no data type
(Nigori and Device Info still run), so nothing is committed before the
chain's passphrase is in place; otherwise a first device's initial commits
would go out under the server-known keystore key. With the engine up: if
the server's Nigori needs a passphrase (a joining device, or keys lost),
`SetDecryptionPassphrase()`, once per time it's asked; if the passphrase
type isn't explicit yet (first device), `SetEncryptionPassphrase()`; once
it's explicit, the wanted types are selected and start. (A
`SyncSetupInProgressHandle` doesn't work for this: it holds Nigori back
too, so the passphrase type never becomes known.)

Any device sets it, not only the one that made the chain: a device that
finds no custom passphrase on the server (a wiped server) must not upload
in the clear, and two devices racing to set it converge, since the
passphrase is the same and the loser is asked to decrypt. A rejected
passphrase (another chain's data on the server) keeps the types held:
nothing syncs, and the page says why.

### Types

No feature flag turns off the unwanted types in 156: Themes, Apps (with
App settings and Web Apps) and Saved Tab Group controllers are built
unconditionally, and `WEBAUTHN_CREDENTIAL` belongs to the Passwords
selectable type. So `CompassSyncService` deselects the selectable types
Apps, Payments (Wallet types, including Wallet Credential), Product
Comparison, Saved Tab Groups and Themes, and a one-line patch passes
`WEBAUTHN_CREDENTIAL` as a disabled type to
`CommonControllerBuilder::Build()` in `sync_service_factory.cc`. The rest
(Sharing, User Events/Consents, Security Events, the Wallet types, shared
tab groups) don't run in local-sync mode anyway.

The user's choices ("What syncs", see "UI") live in a Compass pref,
`compass.sync.disabled_types`, not in `SyncUserSettings`, because the
passphrase hold clears the selection at every start. `SelectTypes()`
selects the registered types minus the unwanted ones minus the switched-off
ones; selectable types without a switch (Cookies) keep upstream's default.

### Change notifications

Chromium learns about remote changes through FCM invalidations, which need
Google's keys, so they're off. Instead:

- Compass calls `SyncService::TriggerRefresh()` (public, no patch) when a
  window gains focus and when the Mac wakes.
- A long-lived Server-Sent Events stream from `CompassSyncNotifier` to the
  server, which calls `TriggerRefresh(types)` the moment another device
  commits.
- The server's poll interval (sent in `ClientCommand`), 15 min, is the
  fallback.

**Server** (`server/events`). `GET /v2/compass/events?client_id=<cache
GUID>`, authenticated as `/v2/command/`: `Authorization: Bearer <token>`
checked by `auth.Authorize` (401), then the disabled-chain check (403). It
answers `200 text/event-stream`, writes `: connected`, a `: keep-alive`
comment every 25 s, and after a commit by another device of the chain:

```
event: changed
data: {"types":[32904,970000]}
```

(EntitySpecifics field numbers, sorted; never contents.) The same stream
also carries `event: room` for the rooms the chain is a member of ("Rooms"
below); clients act on `changed` only and skip other event types, which
is what kept the older ones working when `room` was added. The controller
calls the hub after every successful Commit with the types of the entries
that got `SUCCESS` (a conflict-only commit tells nobody), excluding streams
whose `client_id` is the commit's `cache_guid` (the committer). A stream
without `client_id` hears everything, its own commits too. One hub, in
memory, chains keyed case-insensitively; per stream a set of pending types
and a wake channel, so `Notify` never blocks a commit and a slow stream
gets one merged event. Limits: 20 streams per chain, 50 per client address
(`COMPASS_SYNC_EVENTS_PER_IP`; behind a proxy this needs
`COMPASS_SYNC_TRUSTED_PROXIES`, or every device has the proxy's address),
`429` with `Retry-After: 60` beyond; a write that takes over 15 s ends the
stream. The route is mounted outside chi's 60 s request timeout (which
would cut each stream at 60 s); the rest keeps it. Graceful shutdown:
`http.Server.Shutdown` doesn't end handlers, so `RegisterOnShutdown(hub.Close)`
ends the streams and refuses new ones (503). A restart drops all streams;
the devices reconnect.

**Poll interval.** go-sync sends `ClientCommand.set_sync_poll_interval` in
every response (`HandleClientToServerMessage`, `command/command.go`). Its
constant of 30 s is now a variable, 900 s by default,
`COMPASS_SYNC_POLL_SECONDS` (30 to 86400). Devices without push (older
builds, a proxy that buffers) still sync on focus, wake and every 15 min.

**Proxies.** Caddy's `reverse_proxy` flushes `text/event-stream` responses
at once by itself; the bundled `Caddyfile` also sets `flush_interval -1`.
Behind a proxy, set `COMPASS_SYNC_TRUSTED_PROXIES` to the address the proxy
connects from (with Docker's port mapping, the bridge gateway); otherwise
all devices share one address and the 50-stream limit is for all of them
together. A proxy reload closes streams; devices reconnect in about a
second. nginx needs `proxy_buffering off` (the server also sends
`X-Accel-Buffering: no`). `server/README.md` says the same.

**Browser** (`CompassSyncNotifier`). It holds the stream and calls back
with the changed types. Transport: `network::SimpleURLLoader` with
`DownloadAsStream()` and a `SimpleURLLoaderStreamConsumer` (in 156, the
upstream way to read a body as it arrives; no timeout set), behind a small
`EventStream`/`EventStreamFactory` interface so the logic is tested on a
fake transport and the real one on `TestURLLoaderFactory`. The parser
follows the HTML spec's event-stream rules for `event` and `data` (LF, CR,
CRLF; comments skipped; 64 KiB line cap). Field numbers map to
`syncer::DataType` with `GetDataTypeFromSpecificsFieldNumber()`; unknown
ones are dropped; an unreadable event refreshes everything.

- **Owner**: `CompassSyncService::UpdateNotifier()`, called on every sync
  state change: creates it once the chain runs, the passphrase is in place
  (`AreDataTypesReleased()`) and the transport is active; destroys it on
  Leave, Reset, sync shutdown and profile shutdown. One per profile with a
  running chain, using that profile's account.
- **Token**: a fresh one per connect (`SyncCredentials::MakeToken`). The
  server checks it at connect only; a stream may stay open past the token's
  24 h, which is fine (the device holds the key).
- **Cache GUID**: the local `DeviceInfo`'s `guid()`, which is the engine's
  cache GUID, the one each Commit carries. Read at each connect; before
  DEVICE_INFO runs it's empty and the device hears its own commits until
  its next reconnect (an extra GetUpdates, nothing worse).
- **Refresh**: `SyncService::TriggerRefresh(kSyncInvalidationsService,
  types ∩ active types)`. A 250 ms debounce merges a burst (another
  device's import commits in batches of 90, type after type) into one
  request, and the engine adds its own 500 ms local-refresh nudge delay.
  Not `kMinRefreshInterval` (10 s): that throttles focus and wake, which
  fire often with nothing new; a notification means there is a change now.
- **Reconnect**: `net::BackoffEntry`, 1 s doubling to 5 min, up to 20%
  shorter (jitter), reset once a stream has lived 30 s; a network change
  (`NetworkConnectionTracker`, any type but NONE) or a wake
  (`PowerSuspendObserver::OnResume`) reconnects after 0.5 s with the
  backoff reset; NONE counts as a failure (the type is only a hint, so it
  keeps trying). A stream silent for 75 s (three keep-alives) is dead (NAT,
  sleep) and replaced; the same timer bounds the wait for a response. HTTP
  errors (401, 429, 503) back off. After every reconnect but the first
  connect, a refresh of all types: changes made while it was down weren't
  announced.
- **Logs** (`--vmodule=compass_sync_notifier=1`):
  `[compass-sync] notifier: connecting to ...`, `connected`, `changed
  <types>`, `refresh <types>`, `<why>; reconnecting in N ms (failure K)`.

With push, a change on one device typically reaches another in about a
second.

**Limits.** Single process: notifications reach only streams on the
process that took the commit (as the rest of the server, in memory). Each
device holds one HTTP connection open (HTTP/2 through Caddy shares it with
sync requests). Today and title throttling still applies on the sending
side: a tab's content change can wait up to 10 s before it's committed,
and only then announced.

## History

History doesn't sync under local sync with a custom passphrase, by
upstream design. Five blockers, and what Compass changes:

| Blocker | Change | Gate |
|---|---|---|
| 1. `HISTORY` and `HISTORY_DELETE_DIRECTIVES` are `kNeverEncrypted` in `data_type.cc` (Google's server reads history for Web & App Activity) | `chromium_src/components/sync/base/data_type.cc` wraps `EncryptableUserTypes()` to add both | none needed: only read with a custom passphrase, under which upstream stops history unless local sync is on (row 3) |
| 2. `LocalSyncSupportPolicy::kUnsupported` for both | same override wraps `LocalSyncSupportedTypes()` | only read with local sync on |
| 3. Their controllers return `kMustStopAndClearData` whenever `IsEncryptEverythingEnabled()`, which a custom passphrase always is | patches to `history_data_type_controller.cc` and `history_delete_directives_data_type_controller.cc`: not when `SyncService::IsLocalSyncEnabled()` | local sync |
| 4. `kDisallowEnterprise` with the account's managed status `kPending` (no account) stops HISTORY | patch to `history_data_type_controller_helper.cc`: passes when `IsLocalSyncEnabled()` | local sync |
| 5. Delete directives are only made when a `WebHistoryService` exists (none with local sync), so a deletion never left the device | patch to `browsing_history_service.cc` (chrome://history "Remove"): also with `IsLocalSyncEnabled()`; patch to `history_service.cc` (Clear browsing data, extensions API): without web history, ask the directive handler anyway | the first: local sync. The second has no `SyncService`; the handler only commits while HISTORY_DELETE_DIRECTIVES runs, and outside local sync that type runs only together with web history, so in practice local sync too |

The `kPreconditionsMet` paths still honour `SavingBrowserHistoryDisabled`
(policy). `data_type.cc`'s table is untouched, so the override doesn't
conflict with the `COMPASS_SPACES` patch to that file; it's the tree's
first `chromium_src` override (`build/redirect_cc.py`, wired by `compass
gen`). Upstream's `DataTypeTest.LocalSyncSupportedTypes` and
`LocalSyncTest.ShouldStart` fail by design (they assert History is out of
local sync). History encryption itself is fine for us: history sync's
server-side features don't apply.

kHistory's other types (USER_EVENTS, JOURNEY, and DICTIONARY when its
feature is on) stay out: local sync doesn't support them.

**Brave** syncs History through an account (route 1), with subclassed
controllers whose `GetPreconditionState()` always returns
`kPreconditionsMet`, swapped in by a `chromium_src` override of
`common_controller_builder.cc`. Its `data_type.cc` override adds HISTORY
(and DEVICE_INFO) to `EncryptableUserTypes()`, but not
HISTORY_DELETE_DIRECTIVES, and it disables URL delete directives,
presumably because those would carry a URL in the clear. Its
`LowPriorityUserTypes()` override makes delete directives low priority and
History regular (go-sync issue #178): on a first sync, directives applied
before the history they delete arrives leave the entries in place. Compass
encrypts both types, so URL directives stay on, and keeps upstream's
priorities (that override isn't gated, it would change every profile).

**Server.** go-sync's history rules work on encrypted entities (the type
comes from the empty specifics field, the ID is the client tag hash,
updates go by tag); `server/server/e2e_history_test.go` covers it. HISTORY
and directives expire `COMPASS_SYNC_HISTORY_DAYS` (default 14, 1-30) after
their last write, purged hourly. At most `COMPASS_SYNC_MAX_HISTORY`
(default 30,000; never above `COMPASS_SYNC_MAX_ENTITIES`) history items per
chain in that window, counted in four periods of a quarter of the
retention each; past it the server drops new history silently (answers
SUCCESS) so other types keep syncing. History counts toward the chain's
entity and byte limits. Encrypted visits are about 1-2 KB, so 30k is about
30-60 MB, roughly 2,000 visits a day for the whole chain at 14 days.
30 days is the cap because devices keep delete directives on the server
for 30 days (below).

**Deleted history.** The server can't delete the HISTORY entities a
directive covers (it can't read either), and `HistorySyncBridge` keeps no
metadata for synced visits (it untracks them after each commit), so it
can't commit visit deletions without tracking every synced visit again, a
large change to an upstream bridge. Instead the directives stay: upstream
deletes the directives a device processed at its next start (act-once),
which would let a later device get the visits without the directive. Under
local sync, a directive whose end time is less than 30 days old stays on
the server; older ones go as before (`IsCompassKeptDirective()`). So while
a visit can be on the server, so can the directive that deletes it, and a
device that joins later applies it. A kept directive is applied again at
every start (deleting nothing new: its times are fixed). Three patches,
gated on local sync: `delete_directive_handler.{h,cc}` (the setter and the
split at startup) and the directives controller (sets it when
`IsLocalSyncEnabled()`).

The server purges directives after the history retention, while a device
may delete its copy later: a delete of an entity the server no longer has
answers `SUCCESS` (go-sync answered `CONFLICT`, retried forever).

**Consequences.**

- Only visits made while History syncs are uploaded (upstream: local
  history is never uploaded), and a device joining sees the other devices'
  visits within the server's retention at most. Each device keeps what it
  received for its own local retention (90 days).
- On a joining device a directive can arrive before the visit it deletes
  and apply to nothing; the visit then shows until that device's next
  start, which applies the kept directive again. Brave's priority swap
  would close that gap, but it isn't gated.
- A device offline for longer than the retention misses directives.
- sync-internals shows History with 0 entries although visits sync
  (history entities aren't kept as live entries).

## The `COMPASS_SPACES` data type

`syncer::COMPASS_SPACES`, the last user type. `EntitySpecifics.compass_spaces`
is **field 970000** (upstream's recent fields are 1.4M-1.7M, its
deprecated ones lower; 970000 is unused and far from both).
`DataTypeForHistograms::kCompassSpaces = 200`, kept as `kMaxValue` (on a
rebase, upstream's new value goes below it and `kMaxValue` stays ours).
Encryption policy "encrypted if custom passphrase" (always, with the
chain), local sync supported (route 2 needs it), regular priority, medium
nudge delay.

### The proto

`components/sync/protocol/compass_spaces_specifics.proto` is the
reference. It's built by `//components/sync/protocol` (its
`protocol_sources.gni` lists it by its `//compass` path; `proto_in_dir` is
`//`, so the import path is `compass/components/sync/protocol/...`).
`CompassSpacesSpecifics` has a oneof of six entities:

- `CompassSpace` (1): one per space. UUID, name, color, icon,
  `directory_position`, `profile_uuid`, `archive_after_seconds`, and the
  theme fields (see "Space themes").
- `CompassNode` (2): one per sidebar node. UUID, `space_uuid` (empty for
  favorites), `parent_uuid` (empty = the root of its section), `section`,
  `position`, `type`, URL, title, custom title, `collapsed`,
  `last_active_time`, `archived_time`, `saved_url`/`saved_title` (the
  page a pinned tab goes back to), and a folder's `name_source` (1 OPENER,
  2 CLAUDE; unset is the user's name, which Claude never changes:
  docs/design/ai-sidebar.md).
- `CompassProfile` (3): one per profile of the chain, in the home
  profile's account (see "Profiles").
- `CompassFavicon` (4): one per host (see "Favicons").
- `CompassMemory` (5): one per fact Claude keeps about the user, in the
  account of its profile: UUID, text, `create_time`, `update_time` (the
  later edit wins). The bridge diffs `Delegate::GetLocalMemories()` with
  the mirror, and hands remote ones to `Delegate::ApplyMemories()`; while
  the profile's memory isn't loaded, the account's are left alone. See
  docs/design/claude.md, "Memory".
- `CompassLinkRule` (6): one per link rule of the profile (Arc's Air
  Traffic Control): `uuid` derived from the domain (as a favicon's from its
  host, with the prefix `compass-link-rule:`), the normalized `domain` and
  the `space_uuid`. Kept while its space is missing; not uploaded for a
  space that stays on this device; one without `space_uuid` is ignored
  and left on the server. See docs/design/link-routing.md, "Sync".
- `CompassBoost` (7): one per Boost (per-site CSS and JavaScript) of the
  profile, in its account: `id` (a UUID, the client tag), `name`, `match`,
  `css`, `js`, `js_world`, `enabled`, `author`, `create_time`,
  `update_time` (the later edit wins). Handled as memories are
  (`Delegate::GetLocalBoosts()`, `ApplyBoosts()`; nothing deleted while the
  file isn't read; a deleted Boost is a deleted entity); Android leaves
  them alone. See docs/design/boosts.md, "Sync".
- `CompassEasel` (8), `CompassEaselObject` (9) and `CompassEaselAsset`
  (10): an Easel board, one entity per object on it, and one per image
  (docs/design/easels.md, "Sync"). Boards and objects: `uuid` is the client
  tag and the later `update_time` wins, so two devices editing different
  objects of one board both keep their edits. Images: the `uuid` is derived
  from the image's SHA-256 (prefix `compass-easel-asset:`, as a favicon's
  from its host), and they never change, so the bridge skips an image the
  account has without comparing its bytes, and asks
  `Delegate::GetEaselAssetData()` for the bytes only of one it doesn't
  (`GetLocalEasels()` hands images over without them). Otherwise handled as
  Boosts (`ApplyEasels()`; nothing deleted while the store isn't read; a
  deleted board, object or image is a deleted entity). Desktop and Android
  share `EaselStore` and its glue (`browser/easels/easel_sync.h`); iOS has
  its own port.

Choices: proto2 with `optional` fields, as all sync protos; the enums are
`int32` fields with 0 = unset (`section` 1 PINNED, 2 TODAY, 3 FAVORITES, 4
ARCHIVE; `type` 1 TAB, 2 FOLDER, 3 SPLIT; `color` the palette index),
because proto enums need `ProtoEnumToString()` overloads (one more upstream
patch) and an unknown proto2 enum value would read as the default.
Positions are `sync_pb.UniquePosition` messages (as bookmarks use), so a
move is one entity update, and concurrent inserts in the same folder both
survive. Times are microseconds since the Windows epoch. Values another
client sets that this one doesn't use are kept.

Client tag = the existing UUID (`SidebarNode::id()`, the space's ID).
Node IDs are saved (`"id"` per node in `Compass Sidebar.json`;
sidebar_serializer); a missing, invalid or (within its space) duplicate ID
gets a fresh one. `SpaceModel::AddTab()/AddFolder()` take an optional ID,
and `AddSplit()` makes an empty split with one (loading and sync add the
panes), so splits keep theirs too.

### Always on with the chain, gated by a switch

`COMPASS_SPACES` is not an upstream selectable type: that would mean
`UserSelectableType::kCompassSpaces` and its cases in eight upstream files
that switch on the enum. Instead it's in `AlwaysPreferredUserTypes()`, and
its controller (`SpacesDataTypeController`, `compass_spaces_sync.cc`)
returns `kMustStopAndKeepData` from `GetPreconditionState()` unless the
profile's `CompassSyncService::AreDataTypesReleased()`: the chain runs, the
passphrase is in place, and the Spaces switch is on. `HoldTypes()`,
`SelectTypes()`, `Leave()` and the switch call
`DataTypePreconditionChanged(COMPASS_SPACES)`. So it never commits before
the passphrase. Off keeps the metadata, and turning it back on resumes
where it stopped (upstream's selectable types instead clear their metadata
and do a fresh merge).

**Controller registration.** `//chrome/browser/sync` can't depend on
`//compass/browser/sync` (which depends on it), so a tiny target,
`//compass/components/sync:controllers_hook`, holds a factory callback:
`CompassSyncServiceFactory`'s constructor (run with the other factories,
before any profile) registers it, and `CreateControllers()` in
`sync_service_factory.cc` appends what it returns (3 lines and an
include). The controller's delegates are `ProxyDataTypeControllerDelegate`s
whose provider creates the bridge on first use: nothing is built for a
profile that never syncs.

### The bridge

`CompassSpacesSyncBridge` (`browser/sync/`, `:spaces_bridge`, no window
code) is a `DataTypeSyncBridge`, one per profile, owned by the profile's
`SpaceService` through `SpaceService::Attachment`, with a small holder
(`SpacesSyncHolder`, `compass_spaces_sync.cc`) that supplies what needs
windows: deleting a space (`compass::DeleteSpace()`, which switches windows
away first) and moving entries across spaces with their live tabs
(`compass::MoveEntry()`).

- A **mirror**: each entity's specifics as the server last had them, in the
  bridge's `DataTypeStore` (data records next to the metadata, LevelDB in
  the profile). `Compass Sidebar.json` stays the source of truth for the
  tree.
- **Local changes** aren't tracked one by one. Any change to the spaces
  posts one pass that computes every entity from the model and `Put()`s
  those that differ from the mirror, `Delete()`s mirror entities the model
  no longer has. Simple and robust (it also catches changes made while sync
  wasn't running); a few hundred nodes per pass is cheap.
- **Remote changes** go into the mirror first, then into the model through
  its normal methods. The next pass finds the model equal to the mirror, so
  nothing echoes; what it does commit are the model's fix-ups. The
  bridge's `applying_remote_` keeps its own observers quiet during the
  apply; other observers (store, UI, tab strip) react as to any change.
- **Order**: a pass keeps the positions of the longest run of siblings
  already in increasing order and gives the others new ones between their
  neighbors (`internal::AssignPositions()`), so a move commits one entity.
  Suffixes come from the client tag hash, as bookmarks do. Positions the
  bridge keeps aren't re-encoded, so they compare equal.
- **Spaces**: the order is the switcher's (`SpaceDirectory`), kept equal to
  the profile's `SpaceList` order, both ways.
- **Entities this version can't place** stay in the mirror and are never
  deleted from the server; a node waits for its space and appears when the
  space does.
- Leaving sync (`ApplyDisableSyncChanges`) clears the mirror and metadata,
  keeps the spaces.
- **Logs** (`--vmodule=compass_spaces_sync_bridge=1`): `[compass-sync]
  spaces:` "model ready", "applied N entities (initial merge)", "applied N
  remote changes", "committing N changed, M deleted".

### Apply rules

Applying remote changes keeps `SpaceModel::CanMove`'s invariants:

- **Unknown parent** (child arrived before its folder): the node waits at
  the end of its section's root, not committed (it's "parked" as long as
  the mirror names a parent the model doesn't have), and moves in when the
  parent arrives. Parent deleted: it stays at the root and that's
  committed.
- **Cycle** (A moved into B on one Mac, B into A on the other) or
  **invalid placement** (folder in a split, split in a split): the
  section's root, committed. Concurrent moves converge with nothing lost.
- **Conflicts**: per entity, the last commit to reach the server wins
  (engine default). A rename on one Mac and a move on the other of the same
  node: the later write wins both fields. Acceptable; field-level merge
  later if it bites. For a local change that meets a remote delete,
  `ResolveConflict()` keeps upstream's choice (local wins, so the entity
  goes up again) and logs `kept <id>, deleted elsewhere while changed
  here`.
- **Deletes**: removing an entry or space deletes it everywhere. Deleting
  a folder deletes its subtree (one tombstone per node, the sender's pass
  sends them all); entries added here to a folder deleted elsewhere are
  kept at the root. A split the batch leaves with fewer than two panes is
  dissolved.
- **Pinning and unpinning** are moves of the same entity between PINNED and
  TODAY.
- **Live tabs**: a remote URL change of an entry updates the entry only (no
  navigation); a remote delete of a live entry closes its tab
  (`TabStripBridge` does, as for a local delete), except as "Closing"
  below says.

### Today across devices

Today changes far more often than Pinned (every navigation changes a URL
and title), and its entries are often live on one Mac and unloaded on the
other. Each space's Today root is section TODAY (folders, tabs, splits,
nested folders), synced in the same pass as Pinned with the same placement
rules.

- **What a Today entry means elsewhere**: a tab open on Mac A shows on
  Mac B as an unloaded entry; clicking it loads the URL there (a new
  `WebContents`; page state doesn't travel). Both Macs can have it live.
- **New tab page and blank tabs** (`IsRestorableURL()`) and splits with
  such a pane stay local until they show a page; an entry synced once stays
  synced whatever it shows.
- **Last-active times**: applying takes the later of the local and remote
  times, so each device's archiver goes by the latest use anywhere. A
  device commits its time once it's a minute past the server's (the delays
  are hours), so switching tabs doesn't commit every time. The archiver
  also stamps the tab each window shows (`ArchiveIdleTodayEntries()`), so
  an entry left on screen for hours on one Mac keeps a fresh synced time
  and the other Macs don't archive it.
- **Throttling**: per entity, if a tab's only change since the server's
  copy is its URL, title or last-active time, and this device committed the
  entity less than 10 s ago (`kContentCommitInterval`), the pass leaves it
  and a timer runs a pass when the 10 s are up; the latest state then goes
  once. Anything else (new, deleted, moved, pinned, archived, renamed, a
  custom title, collapse) commits at once and takes the content along. A
  remote change to an entity whose content waits here doesn't overwrite its
  URL and title: the local change goes next and wins (it's the later
  commit).
- **No ping-pong on live tabs**: a remote URL or title change of a live tab
  changes the entry, not the page. `TabStripBridge::UpdateNode()` copies
  only what changed in the tab since it last looked (per tab), so the entry
  follows the tab's next real navigation or title change, which then
  commits as the later write. If both Macs navigate the same entry, the
  last commit wins the entity and the other Mac's tab takes it back on its
  next navigation.
- **Closing**: closing an entry on one Mac closes it everywhere, except a
  Today entry (or a split with a pane) that the focused window shows
  (`IsEntryShownInFocusedWindow()`): it stays, with its tab, and is
  re-created so the other devices get it back. **Under a new ID**: the
  server keeps a deleted entity's client tag, and go-sync refused a new
  entity with a tag in use, which Chromium would retry forever. So the
  bridge copies the entry (fresh IDs), hands the live tabs over
  (`TransferLiveTab()`, through the registry, so nothing closes or reloads)
  and removes the old node. (The server now revives a tombstone instead,
  see "Server", but the new-ID path stays and keeps working.)
- **Tabs closed because a window closed or the app quit** change nothing in
  the model (Today keeps those entries, D14), so quitting Compass on one Mac
  doesn't empty Today on the others.
- **Restored tabs** (D14): `RestoredTabMatcher` takes over the first
  unloaded entry with the tab's URL, synced entries included.

### The archive

An archived entry keeps its node's ID (the tab's, or the split's; a split
with one page worth keeping is archived as that page), and the archive
saves IDs (`"id"` on entries and tabs). So archiving is a move: the entity
goes to section ARCHIVE with `archived_time` (a split's panes too, as its
children); position and last-active time stay as they were.

- On another device the entry and its panes leave Today (closing their
  tabs) and the archived entry is built from the mirror
  (`ApplyArchives()`); an archived split waits for its panes, and is never
  deleted for lack of them.
- Every Mac runs its own auto-archive timer on the synced
  `last_active_time`. Two devices archiving the same entry make the same
  move; the later commit's time stays. An entry in use on the receiving
  device (shown in a window, or playing sound, as the local archiver
  spares) stays in Today, and its next pass moves it back to Today
  everywhere.
- **Restoring makes new nodes** (fresh IDs), so the archived entity is
  deleted and a new Today one created. Removing or clearing archived
  entries, and the local cap of 100, delete them everywhere.
- **30-day cap**: the pass leaves out archived entries more than 30 days
  old (`kArchiveSyncPeriod`), so they're deleted from the server, and a
  timer runs a pass when the next one turns 30 days old. A remote delete of
  an archived entry that's over 30 days old here is ignored (kept locally);
  a younger one is removed. Clocks a few minutes apart can remove, on the
  slower Mac, an entry that was about to expire anyway.
- **Debugging**: `--compass-archive-hour-seconds=N` makes an "hour" of the
  space menu's delay N seconds and runs the archiver every N seconds
  (`GetArchiveHour()`, `space_service.h`); for end-to-end runs only. A
  short delay saved to the file rounds to 0 hours ("never") on restart.

### First sync on a device that already has spaces

Merge (`MergeFullSyncData`), a union. The chain's spaces come first, this
device's after; a local favorite whose URL the chain already has gives way
to the chain's; a joining device's untouched starting space ("Home",
nothing pinned) gives way to the chain's first space, its Today entries
moving there. Same-ID entities (data synced before) are the chain's
version. A joining device's Today tab whose URL the chain's Today of the
same space already has gives way to the chain's (`DedupeToday()`): its
live tab moves to the chain's entry. Later, two devices opening the same
page make two entries, as two tabs would.

**Arc-derived IDs.** Spaces, folders, splits, tabs and favorites imported
from Arc get `ArcDerivedId(Arc's ID)`: a name-based UUID, version 8 (RFC
9562), from SHA-256 of `compass-arc-import:<id>` (not UUIDv5: `crypto/`
deprecates SHA-1). Same Arc data, same IDs, so importing it on two Macs
merges instead of doubling. An ID already in use here (a second import on
the same Mac) gets a fresh one, so a re-import adds copies. The reused
starting space keeps its own ID. A split made from Arc's split gets the
split's derived ID.

### Favicons

An entry that was never loaded on a device (a tab from another device, a
pinned entry after a restart) should still show its site's icon. Fetching
it per device is unreliable, because some sites refuse requests that don't
come from a browser, and others only declare their icon in the page. So
the icon a device got from a loaded page goes through sync, as
`CompassFavicon`.

- **Key.** One entity per host. Its UUID (also the client tag) is derived
  from the host, so it's UUID-shaped like every other key here: the first
  16 bytes of SHA-256(`"compass-favicon:" + host`), with byte 6 =
  `(b & 0x0f) | 0x50` and byte 8 = `(b & 0x3f) | 0x80`, formatted lowercase
  8-4-4-4-12. Vectors: `www.example.com` →
  `c94c25bc-f583-5f0a-88eb-63e59b454926`, `github.com` →
  `b73d689b-d088-5dec-9f35-e41fb59757a0` (`FaviconKeyTests` on iOS; the
  desktop's unit test checks the same).
- **Content.** A PNG of at most 64 px a side and 8 KiB, the host
  (lowercase, `www.` kept), the source's larger side before scaling
  (`source_size`), and `update_time`.
- **Account.** It goes in the account of the profile whose entry uses the
  host. Two profiles with entries on the same host each have their own.
- **Writing.** A device writes only icons it got from a page it loaded: the
  largest of the page's declared icons, or the host's `/favicon.ico` when
  it declares none (what browsers show; on the desktop, the tab's favicon).
  It never writes icons it guessed for a page it didn't load. It writes
  after the page loads, when an entry of that account (any section) points
  at the host and:
  - the account has no icon for the host; or
  - its icon is larger (`source_size`); or
  - it's the same size, the bytes differ, and the account's icon is at
    least 7 days old.

  So two devices with different icons don't take turns.
- **Reading.** A device uses a synced icon for any entry on that host until
  it has its own from a loaded page. On the desktop it's what
  `chrome://favicon2` returns for the entry's URL
  (`FaviconService::SetOnDemandFavicons`, which doesn't overwrite an icon
  the profile already has). On the phone it's `Favicons`, a cache keyed by
  host.
- **Garbage.** A device deletes an icon of its account once no entry of the
  account points at its host and the icon is more than a day old. The delay
  keeps an icon alive while an initial download arrives in several batches,
  with the icon before its entries.
- **Desktop.** The bridge keeps the icons in its mirror only (they aren't
  sidebar nodes, so a pass never deletes one for missing from the model,
  only as garbage). `OfferFavicon()` applies the writing rules and commits
  at once; the pass collects garbage. `SpacesSyncHolder` feeds it:
  `HistoryService::AddFaviconsChangedCallback()` says a page's stored icon
  changed, and if a tab of the profile shows that page and an entry is on
  its host, it reads the largest stored `kFavicon` bitmap
  (`GetRawFaviconForPageURL()`, size 0) and offers it through
  `MakeSyncedFaviconPng()` (`favicon_png.h`: as stored if it fits, else 64,
  48 or 32 px). An icon that doesn't change on a reload isn't stored again,
  so 30 s after sync starts the holder also offers the icons of the pages
  open then. Remote icons go to `SetOnDemandFavicons()` under
  `https://<host>/compass-synced-favicon/<update_time>`, one icon URL per
  version (FaviconService keeps an icon URL's bitmaps once it knows it);
  icons under that path are never offered back.
- **Older clients** ignore the entity: the desktop's `IsEntityDataValid()`
  refuses an entity with none of `space`, `node` or `profile`, and the
  phone's bridge skips specifics without a UUID it knows.

### Space themes

`SpaceColor` is a class (grey, or any opaque RGB tint; the palette is
`kSpaceColorPalette`) and spaces have a theme: gradient, intensity, grain,
appearance and icon (docs/design/spaces.md). All of it syncs, in
`CompassSpace`:

- `color = 3` is the palette index, 1 grey ... 8 green. A color off the
  palette also sets `uint32 color_rgb = 8` (0xRRGGBB), and `color` is then
  the index of the palette color closest to it (never grey), so clients
  from before themes show something close. `color_rgb` counts only while
  `color` is still that closest index: an older client that picks another
  palette color writes `color` and keeps the stale `color_rgb` as received,
  and the palette color wins.
- `repeated CompassColor gradient = 9`, each `{int32 palette = 1; uint32 rgb
  = 2}`: rgb when set, else the palette index (1 is grey, which the model
  drops from gradients).
- `double intensity = 10` (absent = 0.31, the default), `double grain = 11`
  (absent = 0), `int32 appearance = 12` (1 or absent auto, 2 light, 3 dark).
- `string icon = 4`: `emoji:<emoji>` or `symbol:<name>`, empty or absent
  for none.

Defaults are never written. Remote values go through the model's setters,
which clamp and validate; an unreadable color or gradient entry is ignored,
an unknown appearance or icon reads as auto/none and stays on the server
until it's changed here. Writing compares each field, decoded, with what the
entity says, so an equivalent encoding isn't rewritten and an applied remote
theme isn't committed back. Every theme setter fires
`SpaceModel::Observer::OnSpaceChanged`, which `SpaceList` passes on as
`OnSpaceUpdated`, so a theme-only edit commits the space entity on its own
(`CompassSpacesSyncBridgeTest.ThemeOnlyChangesCommit`). The iOS client keeps
the new fields as unknown fields and the icon string as received; it only
reads and writes `color`.

### Spaces that don't sync

A space can stay on this device: "Sync to your other devices" off in its
menu, or made that way with "New Space on This Device" (command bar, the
macOS Spaces menu). `SpaceModel::synced()`, true by default, saved as
`"local_only": true` in `Compass Sidebar.json` (left out when synced); the
flag itself never syncs, so there's no proto field.

- **Nothing of it goes up.** `ComputeLocalSpecifics()` leaves out the space
  and everything in it: Pinned (folders, live folders, splits), Today and
  its archive. Favorites are the profile's, not a space's, and keep syncing.
- **Turning sync off removes it from the other devices** (the menu asks
  first): the next pass finds the space's entities in the mirror but not in
  the model, and deletes them, as deleting the space would there. Here
  nothing changes. Moving an entry from a synced space into a local one
  deletes it elsewhere the same way; moving one out uploads it.
- **Turning it on uploads it** like a new space (same IDs: the server
  revives tombstones, see "Server").
- **Incoming changes never touch it.** Remote entities for a local space,
  for a node in one or for a node whose space is one (a device that edited
  it before hearing of the delete) go into the mirror but not into the
  model, and the next pass deletes them again, so the chain converges on
  the delete. A remote delete of such a space or node is ignored.
- **Order.** Local spaces have no synced position; when remote positions
  reorder the switcher, each keeps its place after the synced space it
  followed (`OrderSpaces()`), and `DirectoryPositions()` drops positions
  of spaces that have none.
- **Open tabs** (`SESSIONS`): tabs of a local space aren't in this device's
  synced session, so other devices' "Tabs from other devices" don't list
  them (`compass::IsInUnsyncedSpace()`, called by
  `TabContentsSyncedTabDelegate::ShouldSync()`, desktop). History still
  syncs: it's the profile's, not the space's.
- **Moving to another profile** keeps the flag.
- **Favicons**: hosts of a local space's entries aren't offered, and an
  icon only a local space uses is collected (`GetEntryUrlsByHost()`).
- **UI**: the space menu's "Sync to your other devices" (a check; turning it
  off wants a second click) shows while the profile's spaces sync, or for a
  space already kept here. The switcher shows a small dot on the space's
  button, and its tooltip says "On this device only".
- **Platforms**:
  - Desktop (macOS, Windows, Linux): all of the above.
  - Android shares the C++ model, serializer and bridge, so a flag set on
    the desktop side of a profile is kept and respected; it has no UI to set
    it yet, and its open tabs aren't filtered (no window_sync there).
  - iOS (`CompassSpacesBridge`, `Space.synced`, saved as `localOnly`): the
    same rules in its change-driven bridge (`desired()` gives nothing for a
    local space's nodes; `localChanged()` deletes a space turned off and
    uploads one turned on; `applyRemote()` drops and deletes again what
    comes for a local space; the initial merge leaves them out). The space
    menu has "Sync to Other Devices", asking before it turns off; the
    header shows `icloud.slash`. Favicons aren't filtered there yet.
- **Tests**: `CompassSpacesSyncBridgeTest.LocalSpaceIsNotUploaded`,
  `LocalSpaceIgnoresRemoteChanges`, `TurningSyncOffDeletesRemotely`,
  `TurningSyncOnUploads`, `LocalSpacesKeepTheirPlace`;
  `SidebarSerializerTest.LocalOnlyRoundTrips`; `SpaceModelTest.SetSynced`;
  `SidebarPageHandlerTest.SetSpaceSynced`;
  `CompassSidebarBrowserTest.LocalSpace` (menu, marker, session sync);
  `CompassMainMenuBrowserTest.SpacesMenu`; iOS `LocalSpaceSyncTests`.

### Patches for a new data type

Chromium has no plug-in point for types, so these are patches (small, but
every rebase touches them):

- `components/sync/base/data_type.h/.cc`: enum value, info table, root tag,
  histogram value, `AlwaysPreferredUserTypes()`, the static_asserts on the
  type count.
- `components/sync/engine/cycle/data_type_tracker.cc`: two exhaustive
  switches.
- `components/sync/base/user_selectable_type.cc`: only its static_assert.
- `components/sync/protocol/entity_specifics.proto` (the field),
  `protocol_sources.gni` (builds the `//compass` proto),
  `proto_visitors.h` (visitors for the Compass messages).
- `chrome/browser/sync/BUILD.gn` and `sync_service_factory.cc` (the
  controllers hook).
- `tools/metrics/histograms/metadata/sync/enums.xml` and `histograms.xml`
  (the type's histogram value and suffix; `DataTypeForHistograms` must
  stay in sync or tests fail).

`proto_value_conversions_unittest.cc`, `sync_service_factory_unittest.cc`
and `sync_test.cc` have the same static_assert but aren't built by Compass;
they'd need the same one-line change if those suites are ever built.

## Profiles

Chromium sync is per profile; Compass spaces are browser-wide and each
belongs to a profile (D17).

**Accounts.** Every profile the switcher lists (`SpaceDirectory`) is in
the chain. The profile that starts or joins it is the **home profile**: its
account is the chain's own (`compass-sync-auth`). Every other profile has
its own:

- `compass.sync.profile_uuid`: a random UUID, made once when the profile's
  `CompassSyncService` is first built (Chromium profiles have no stable ID
  beyond their directory). Mapping at join replaces it.
- `compass.sync.profile_account`: true for every profile but the home.
- Auth key `HKDF(seed, info="compass-sync-auth:" + uuid)`, passphrase
  `HKDF(seed, info="compass-sync-passphrase:" + uuid)`, base64 as for the
  chain's (vectors in "Identity"). Each profile stores the seed itself (the
  home's encrypted value, copied: the os_crypt key is the same for every
  profile), so `SyncCredentials`, `sync_hooks` and the engine don't know
  about profiles.

**Private servers and profile accounts.** A server that serves only the
chains it lists (`COMPASS_SYNC_ALLOWED_CHAINS`) can't know a profile
account's ID in advance, so it would answer it 401 forever (a friend's new
profile, 2026-11-20: its page said "Starting…" and the phone failed that
account). The chain links them: `POST /compass/account/link` with the home
account's token and the profile account's in `X-Compass-Linked-Token`
(`AccountLinker`, `components/sync/compass_account_linker.*`, and the
phone's `AccountLinker`). The server keeps `chain_links {client_id, root}`
(the root is the listed chain, also when a linked chain links another; at
most 64 per root) and lets a linked chain in while its root is listed and
not blocked (`auth.ChainAllowed()`); with sign-in on it also binds it to
the same user, as before. Clients link:
- desktop: a profile account not linked on its server yet
  (`compass.sync.account_linked_server` differs) links itself before its
  engine gets the key, so its first request already passes; and again,
  at most once a minute (else every 5), while the server refuses it.
  `EnrollProfile()` and leaving clear the pref;
- iOS: every profile account once a run as it starts, and again after a
  401 (at most once a minute).
A 401 on an account shows as such, not as "Starting…": the connection
manager records it (`SyncCredentials::SetRefused()`, any other answer
clears it) and the page says "The sync server refused this profile (it may
serve only invited people). Ask the server's owner to allow it." (or "this
device's chain" for the home). The phone shows the same under Settings →
Profiles. Older clients don't link; the server's README says so.

**Passphrase per profile.** Each account derives its own rather than
sharing the chain's. It costs nothing (one more derivation; each profile
has its own Nigori anyway) and buys domain separation: an entity the
server copies from one account into another doesn't decrypt there, so a
malicious server can't move Work's passwords into Personal or replay one
account's data into another. It doesn't protect a profile from someone
with the seed, and isn't meant to: the seed opens every account.

**The profile list.** The home profile's `COMPASS_SPACES` also has one
`CompassProfile {uuid, name, avatar, home}` entity per profile of the chain
(`avatar` is Chromium's default avatar icon index + 1; client tag = the
UUID). The holder asks the home profile's `CompassSyncService` for this
device's profiles (`GetLocalChainProfiles()`), and hands it every profile
entity after a remote change (`OnChainProfiles()`). Rules:

- Names and avatars follow the chain: the local profile with a chain
  profile's UUID takes its name and avatar. A rename here commits.
- Profiles are never deleted through sync: deleting one stays on its
  device (the home remembers it in `compass.sync.profiles_removed` and
  doesn't create it again), and a device without a profile doesn't drop it
  from the list.
- One home UUID: a home that never published its own takes the chain's (a
  device joining by recovery code); two published ones (two devices joining
  at once) keep the smallest, and the other's entity is deleted.
- A profile of the chain this device doesn't have is **created**
  (`MissingProfiles`, `CompassProfileCreator`) and listed in the switcher,
  which keeps it loaded; its account then fills it. The device's own new
  profiles join the chain as new profiles. `compass.sync.profile_dirs` on
  the home remembers which directories are the chain's profiles, so one
  that isn't loaded isn't created again. That map alone isn't enough: it is
  cleared on leaving, and a creation that crashed is never recorded. So
  before creating anything, the home reads every profile directory's sync
  prefs once per run (`ScanProfileDirs()`, `PlanProfileDirs()`): each chain
  UUID maps to one directory (listed, else loaded, else recorded, else the
  current chain's), and sync-made copies that are neither listed nor loaded
  are marked ephemeral and omitted, so Chromium deletes them, unloaded, as
  the next launch starts.

**Restart behaviour.** As for one profile, a profile already loaded when it
joins the chain syncs from the next launch, and a profile made locally
while syncing ("New profile…") syncs after the next restart; its page says
so. A new profile made **for a space** (Assign to profile → a new one,
`AssignSpaceToProfile()`) doesn't wait: while the chain runs here, the home
makes it through `CompassProfileCreator` with a new UUID
(`CompassSyncService::CreateProfileInChain()`, set as window_sync's
`SetNewProfileMaker()`), as below, so it syncs at once; without a running
chain it's a plain profile. Profiles **created by sync** don't wait either: the creator writes the new
directory's `Preferences` before the profile loads (the seed as the home
stores it, server, UUID, `profile_account`,
`sync.enable_local_sync_backend` and its directory), then
`ProfileManager::CreateProfileAsync()` loads it, so its sync service starts
in local-sync mode against its account. So a device joining by recovery
code restarts once and then gets Work, already syncing. Ruled out:
unloading and reloading a profile to rebuild its sync service (the
profile's prefs commit and the new instance's read race on different
sequences, and it needs every keep-alive gone), and a pref store hook
before keyed services (no hook between prefs loading and
`CreateBrowserContextServices()` without a patch). The cost of pre-written
prefs: the profile isn't "new" to Chromium (`IsNewProfile()` reads the
Preferences file's existence), which only affects first-run niceties.

**Joining with profiles (the mapping step).** The joining profile becomes
the home here, taking the chain's home UUID. The device's other listed
profiles are mapped to the chain's: each takes over one (its UUID, so its
account; the user's choice) or joins as a new one. Default: the first
untaken chain profile with the same name (ASCII case and outer spaces
ignored), else new. Chain profiles nobody takes are created after the
restart.

- Pairing: message 4 carries the list. With other profiles here, the join
  waits for the mapping before saving anything ("Choose how this Mac's
  other profiles sync, then Continue.").
- Recovery code: the list isn't known before the first sync, so the
  joining profile becomes the home at once and, if other profiles exist,
  `compass.sync.profiles_pending` holds them back; once the home's first
  sync brings the list, the page shows the same mapping, and confirming
  puts them in the chain (they sync after a restart) and creates the
  missing ones (at once).
- The logic is `CompassSyncService::GetProfileMapping()` /
  `SetProfileMapping()` behind `SyncController`; mapping itself is pure
  functions (`profile_list.h`): `DefaultProfileMapping()`,
  `IsValidProfileMapping()`, `ProfilesToCreate()`.
- Not offered: mapping the joining profile to anything but the home, or
  leaving a chain profile out on this device.

**Spaces follow their profile.** Each profile's bridge syncs its own
`SpaceList` into its own account.

- `MoveSpaceToProfile()` keeps the space's ID (and so its place in the
  switcher). The old profile's bridge deletes the space and its nodes from
  its account; the new one's creates them under the same IDs (a revive on
  the server, see "Server").
- On another device the two changes come in either order. Create first:
  the new profile's bridge finds the space in the old profile here and
  moves it over with its entries (`Delegate::AdoptSpace()` →
  `MoveSpaceToProfile()`), then applies the entities to it; the old
  profile's next pass deletes it from its account. Delete first: the space
  goes, then comes back in the new profile, without its live tabs.
- `CompassSpace.profile_uuid` is set (informational: the account decides).

**The browser-wide order across accounts.** A bridge assigns its spaces'
`directory_position`s over the whole switcher
(`Delegate::GetDirectoryOrder()`): every listed space in order, with the
positions the other profiles' bridges synced for theirs (a browser-wide
registry fed by `OnSpacePositions()`), so its own fall between theirs and
compare across accounts (`UniquePosition`s are plain sortable bytes).
After a remote change, the switcher sorts the spaces with known positions
by them, in their slots (`SortDirectoryByPositions()`), before the bridge's
own pass computes positions from it. A switcher change schedules a pass in
every profile's bridge.

**Per profile or device-wide.**

- **Switches are per profile**: `compass.sync.disabled_types` is a profile
  pref and each profile's page shows and sets its own, the way Chromium
  keeps sync settings per profile. A profile enrolled or created by sync
  starts with every switch on. The chain's profile list rides on the home
  profile's `COMPASS_SPACES`, so with the home's Spaces switch off the list
  neither goes up nor comes down.
- **Leave** leaves with every profile of the device (each profile's seed
  and account flag go; their data stays).
- **Reset** is device-wide, like Leave: `ResetChain()` from any profile's
  page stops every loaded profile of the chain (`StopForReset()`), saves
  one new seed, gives every one of them that seed and runs
  `ForgetOldChain()` on each. Each profile keeps its UUID and role, so the
  home stays the home and the others keep their own accounts, now derived
  from the new seed. Resetting only one profile would split the device
  across two chains.
- **The notifier is per profile**: each runs its own stream with its
  account's token, so the server's per-address limit (50) is devices ×
  profiles behind one address.
- **Devices** come from the profile's own `DEVICE_INFO`, i.e. the devices
  that carry this profile. **Remove** removes the device from the shown
  profile's account, and from this device's other profile accounts when it
  can tell the same device there (see "Removing a device"). **Recovery code**: the same seed in every
  profile, so any profile's page shows the chain's 12 words. **Add a
  device** from any profile sends the home's list.
- **Logs**: the `state=` lines end with `profile=<directory>`, and
  "passphrase in place, data types start (<directory>)", "chain … for
  <directory> (home profile | profile <uuid>)", "the chain has N
  profile(s)", "profile X joins the chain as <uuid> (after a restart)",
  "creating profile …" (vmodule `compass_profile_creator`).

## Server (`server/` in this repo, Go)

- Forked from **brave/go-sync** (MPL-2.0: fine for a fork; modified files
  stay MPL). It implements the protocol (`GetUpdates`, `Commit`, progress
  markers, `ClientCommand`, quotas).
- Its DynamoDB + Redis storage is replaced with **SQLite** (one file, one
  binary, a Docker image) so self-hosting is `docker run`. Postgres later
  if a shared instance needs it.
- Its auth is the Ed25519 token above, unchanged.
- Endpoints: `POST /v2/command/` (protocol), `/v2/compass/pair` (pairing),
  `GET /v2/compass/events` (change notifications), `GET /v2/compass/quota`
  (limits), `/v2/compass/push` (push tokens), `GET /v2/compass/info`,
  `/v2/compass/account` and `/v2/compass/login` (company sign-in, see
  "Company sign-in (OIDC)"), `DELETE /v2/compass/devices/{cache_guid}`
  (removing a device, see "Identity"), `/v2/compass/rooms`,
  `/v2/compass/identity` and `GET /v2/compass/directory` (see "Rooms"),
  `GET /healthz` and `/health-check`.
- Admin commands (`compass-sync-server`): `backup`, `users ...` and
  `chains list` (see "Company sign-in (OIDC)"), `devices list CHAIN` (cache
  GUID, name and OS from the unencrypted `DEVICE_INFO`, last updated,
  active or removed) and `devices remove CHAIN CACHE_GUID` (as the
  endpoint; the running server's poll cache sees it within 10 min), and
  `rooms list`, `rooms show ROOM`, `rooms delete ROOM` (see "Rooms").
- It stores specifics opaquely (it can't read them anyway) and only
  interprets envelope fields, so types newer than its protos sync.
- Ships a `docker-compose.yml` with the server and a Caddy front for HTTPS,
  and `server/README.md` (VPS, home server behind Tailscale, backups,
  proxies). `compass sync-server` runs it locally for development.
- Anyone can run their own. The project also runs a public instance;
  everything on it is encrypted on the devices. The desktop's server field
  has no default; the iOS app's sync field and join form start with the
  `COMPASS_SYNC_SERVER` build setting, empty unless the build sets it.
  Compass for iOS can also start a chain and add devices (ios.md,
  "Settings").

**Create over a tombstone.** A commit of a new entity (version 0) whose
client tag belongs to a deleted entity of the same type revives that
entity: same server ID, new version, the create's content, undeleted
(Chromium's servers and LoopbackServer do the same; a saved-again password
or a space put back under its old ID). Over a live entity it is a
CONFLICT. go-sync instead stored a second entity with a new ID next to the
tombstone (and answered CONFLICT forever if a delete had left the tag item
behind). History doesn't use tag items (its ID is the tag), so it's
unaffected.

**Limits.**

| Limit | Server answer | What the engine does with it |
|---|---|---|
| 100k entities per chain (`COMPASS_SYNC_MAX_ENTITIES`) | the entry gets `OVER_QUOTA` | treats it as transient: backoff and retry |
| 200 MB per chain (`COMPASS_SYNC_MAX_BYTES`) | the entry gets `OVER_QUOTA` | same |
| History quota (see "History") | `SUCCESS`, the visit is dropped | nothing to see |
| 50 devices per chain | `THROTTLED` for the new device's GetUpdates | throttles every type (`SyncStatus::throttled_types`) |
| SSE streams (20 per chain, 50 per address) | `429` on `/compass/events` | the notifier backs off; polling still works |
| Pairing joins (10 per IP per 10 min) | `429` | |

There's no rate limit on `/command/` (go-sync never had one).

**Quota shown.** An entry's error message isn't passed on
(`FailedCommitResponseData` keeps the type only), and a top-level
`THROTTLED` with the over-quota types would change how the engine commits
(it skips throttled types, also for their deletes). So the server keeps
telling the engine what it always did, and Compass asks the server
separately:

- Each chain remembers which limit refused its commits (`chains.quota_full`,
  bits entities/bytes/history, with the time). A commit that stores a new
  entity clears entities and bytes; a stored visit clears history.
- `GET /v2/compass/quota` (`server/quota`), signed like `/command/`,
  answers the counts and limits and `"full": [...]`: a remembered refusal,
  or live counts at a limit (bytes within 64 KiB of it).
- `CompassSyncService` asks after a sync cycle: at most every 30 min,
  every minute while the last commit failed or a limit is in the way, and
  at the first cycle after a start (`QuotaFetcher`). An older server's 404
  is no answer and changes nothing. Each profile asks with its own token.
- `SyncStatus` has a `SyncLimit`: server full (entities or bytes) or
  throttled turn an "on" status into an error with its own message; history
  full keeps it on and adds a sentence. The menu row says "Sync: server
  full", "Sync: paused by server" or "Synced (history full)". Other errors
  (can't reach the server, a wrong key) come first.
- Log: `[compass-sync] quota: entities N/M, bytes N/M, history N/M (D
  days), full: ...`.

The history cap is a soft warning, not an error: the server still answers
`SUCCESS` (so other types keep syncing and the engine doesn't retry
forever), but the user should know their newer visits stay on one Mac.

**Tombstone cleanup.** Deleted entities whose last change is older than
`COMPASS_SYNC_TOMBSTONE_DAYS` (default 30; 0 keeps them) are purged with
the hourly expiry. A device away longer doesn't learn of those deletions
and keeps its copy. If it then commits an update for one, the server
re-creates the entity (when it has a client tag, which all Compass types
and modern bookmarks have), as Chromium's conflict rule would (local edit
wins over remote delete); without a tag it's a conflict.

**Old chains.** Reset leaves the other devices on the old chain until they
join again, so deleting it at reset would cut them off at once; and a
delete request from the old identity would need a new C++ path signed with
the old seed before it's replaced. Instead the server records when a
device of each chain last talked to it (`chains.last_active`, written at
most hourly per chain, by the `/command/` middleware) and, hourly, clears
and disables chains idle for `COMPASS_SYNC_IDLE_CHAIN_DAYS` (default 180;
0 keeps them), as go-sync's `ClearServerData` does. A device coming back to
an expired chain gets `DISABLED_BY_ADMIN` and stops syncing; its local data
stays. A reset chain's old data so goes 180 days after its last device
leaves.

**Schema.** `PRAGMA user_version` 2 adds the columns for the quota state
and last activity; 3 adds `oidc_users` and `chain_logins` (company
sign-in); 4 adds `removed_devices` (`client_id` lower-case, `cache_guid`
compared exactly, `removed_at`; deleted with the chain's data by reset,
idle-chain expiry and `users revoke --delete-data`); 5 adds the room
tables and `identity_keys` (see "Rooms"); older databases migrate on
start. An older server refuses
a newer database, so take a backup before updating (the update script
does).

**Backups.** Every `COMPASS_SYNC_BACKUP_INTERVAL` (default 24h; 0 off) the
server writes `VACUUM INTO` copies to `COMPASS_SYNC_BACKUP_DIR` (default
`backups/` next to the database, so `/data/backups` in the image), named
`compass-sync-<UTC time>.db`, under a temporary name renamed when complete,
keeping `COMPASS_SYNC_BACKUP_KEEP` (default 7). The first one is due an
interval after the newest on disk, so restarts don't add backups. Restore:
`server/README.md`, "Backups".

**Deploying.** `server/deploy/update-remote.sh --host H [--port P]
[--user U] [--dir D] [--service S] [--local-port 8295] [--public-url URL]
[--dry-run]`, with the operator's server as `--host`/`--port`: refuses
uncommitted `server/` changes, takes a backup through the running server
(`backup -`) into `D/backups` (a tar of `D/data` if it isn't running),
rsyncs `server/` to `D/src`, `docker compose build` with
`COMMIT`/`VERSION` = the short SHA and `BUILD_TIME`, `up -d`, then waits up
to 60 s for `/health-check` to name the commit and `/healthz` to say `ok`,
and checks the public URL. ssh uses the caller's keys; no secrets in the
script.

## UI

Upstream's sync settings are replaced by a page of our own,
`chrome://compass-sync`, also the Sync section of compass://settings
(settings.md). It uses colors.css tokens, so light and dark follow the
browser.

- **Off**: the server field is empty, with only a placeholder
  (`https://sync.example.com/v2`); there's no default server. "Start sync"
  shows the 12 words once ("I saved it"); "I have another device" takes the
  server and pairing code; "Use a recovery code" under it.
- **On**: the status with a colored dot (last synced, or the error in plain
  words); **Devices** from `DEVICE_INFO` (`DeviceInfoSyncService`'s
  tracker: name, kind from OS and form factor, "active now / 5 minutes ago"
  from the last update, "This device" first; this device's entries from an
  earlier setup, which `IsRecentLocalCacheGuid()` recognizes, are hidden);
  **Remove** on each other device's row (see "Removing a device");
  **Add a device** (code and QR, then Allow/Deny with the device's name);
  **What syncs**, nine switches; **Recovery code**, behind device
  re-authentication; the server, read-only ("leave sync and join again" to
  change it); **Leave sync** and **Reset sync**, each behind a confirm
  dialog (`<dialog>`, not `window.confirm`, so the end-to-end helpers can
  drive it).
- The page re-renders on every `SyncController` change, device list changes
  included (`DeviceInfoTracker::Observer`).

**Switches.** Passwords, History, Bookmarks, Autofill, Extensions,
Settings (kPreferences), Reading list, Open tabs (kTabs) map to upstream
`UserSelectableType`s (`browser/sync/sync_settings.h`); Spaces is
`COMPASS_SPACES` (see "Always on with the chain, gated by a switch"). A
switch changed while the types are held only writes the pref: nothing is
selected before the passphrase is in place, and the release reads the
pref. Upstream's deselection of a selectable type clears that type's
metadata, so a switch turned back on does a fresh merge for it; the Spaces
switch keeps its metadata.

**Recovery code.** "Show recovery code" asks the controller only after
`device_reauth` says yes, as the password manager does before showing
passwords: `ChromeDeviceAuthenticatorFactory::GetForProfile()` with the
page's window, `AuthenticateWithMessage("show your sync recovery code")`
(macOS: "Compass is trying to show your sync recovery code", Touch ID or
the password). With no biometrics and no screen lock it doesn't ask (as
Chrome); on Linux there's no device authentication. `CompassSyncUI` owns
the authenticator; the page handler takes the re-authentication as a
callback, so its tests use a fake.

**Reset sync.** A new seed, so new keys and a new account on the same
server; the other devices stay on the old chain until they join again (as
Brave). It takes a restart, like start and leave: this run's engine stops
(credentials cleared, types held), the new seed replaces the old, and this
device's sync transport data is cleared (the local-sync account's
`SyncTransportDataPrefs`, keyed to the empty Gaia ID). At the next launch
the engine finds no cache GUID, makes a new one, and every type's
metadata, Nigori's included, is dropped on the GUID mismatch (upstream's
own "start from scratch" path), so the data goes up to the new chain as a
first sync. A pref, `compass.sync.reset_pending`, keeps the types held
until this run has set (or had accepted) the new chain's passphrase: the
passphrase type the engine reports first could be the old chain's, cached.
The old chain's data stays on the server until it expires ("Old chains").

**App menu.** The header's ⋯ menu is upstream's `AppMenuModel`, and nothing
in it is hookable from Compass, so one upstream patch
(`chrome-browser-ui-toolbar-app_menu_model.cc.patch`) adds a row above
Settings when `compass::GetAppMenuSyncLabel()` gives one ("Synced",
"Syncing…", "Sync error", "Sync is off", "Sync: restart to finish", and the
quota labels; none in incognito or guest), with the sync icon, and makes
Compass commands enabled in the menu. Its command, `kOpenSyncPageCommand`
(39951), runs through the command controller's existing Compass hook and
opens compass://settings/sync (`chrome::ShowSettingsSubPage()`, which
reuses an open settings tab). The menu is rebuilt each time it opens, so
the label is current. The declarations live in a dependency-free target
(`//compass/browser/ui:app_menu_hooks`), which the toolbar's `:impl`
depends on; definitions in `//compass/browser/ui`. For the end-to-end
helpers the page puts the same label in `<html data-menu-label>`.

**Command bar.** "Tabs on other devices": for a non-empty query, the
other devices' open tabs (`SessionSyncService`'s `OpenTabsUIDelegate`,
`GetForeignSessionTabs()`: recent first, unsyncable and new tab pages
pruned) whose title, URL or device name contains every word, at most six,
grouped by device, the most recently active first; subtitle "On <device> ·
<URL>". Enter opens the URL as a new Today entry of the window's space, in
either mode. Nothing when Open tabs doesn't sync. A `ResultKind`,
`kOtherDeviceTab`; rows show the page's favicon. See command-bar.md,
"Results".

### Removing a device

The device list has **Remove** on every row but "This device" (desktop
page and Android screen alike, both over `SyncController::RemoveDevice()`).
It asks first (a `<dialog>` on the desktop, an `AlertDialog` on Android):
"Remove <name>?", "It stops syncing and disappears from your devices.
What's on it stays there, and it can join again with a pairing code.
Someone who has your recovery code can still join: to lock everyone out,
use Reset sync." (Android: "… use Reset sync on a computer", having no
Reset), Remove / Cancel; an error shows under the list.

- **Request**: `DeviceRemover` (`components/sync/compass_device_remover.h`)
  sends `DELETE <server>/compass/devices/<cache GUID, percent-encoded>
  ?client_id=<this profile's cache GUID>`, signed with the account's token
  (see "Identity" for the server's side). 2xx is done, 409 this device,
  401 the key (or the sign-in, with `X-Compass-Login`), 403 with
  `X-Compass-Device: removed` this device itself was removed, 404/405 an
  older server; anything else "try again".
- **The list** carries each entry's cache GUID (`DeviceInfo::guid()`). A
  removed GUID is hidden at once (`removed_guids_`, for the run), and the
  service asks for `DEVICE_INFO` updates; the server's tombstone, announced
  on `/compass/events`, then drops it from the tracker.
- **Other profiles.** Each profile of a device syncs with its own account,
  so the device has one `DEVICE_INFO` entry, and one cache GUID, per
  profile, and nothing in `DEVICE_INFO` is shared between them
  (`signin_scoped_device_id` is "local_device" under local sync on the
  desktop, and per profile elsewhere). `FindSameDevice()`
  (`browser/sync/sync_settings.h`) matches on name, OS, form factor,
  manufacturer, model and hardware class, and only when exactly one entry
  of the shown account and exactly one of the other account have that
  signature; then the other account's entry is removed too (after the
  first succeeded, best effort). Two alike devices (two phones of one model
  with the default name) aren't told apart: only the shown account's entry
  goes, and the device's other profiles keep syncing until removed from
  their own pages. Only loaded profiles are covered.
- **On the removed device.** Every request it makes with its `client_id`
  gets 403 `{"error":"device_removed"}` with `X-Compass-Device: removed`.
  The engine's posts go through a wrapping `HttpPostProvider`
  (`WrapHttpPostProviderFactoryForDeviceState()`) that notes it in the
  profile's `SyncCredentials`; the notifier, the quota fetcher and Android
  push registration report it too. The service then keeps
  `compass.sync.device_removed`, stops the notifier, push, quota and
  pairing, and the connection manager stops posting (it answers the engine
  403 itself, so the engine backs off without traffic, this run and the
  next ones). The status is an error: "This device was removed from sync
  on another device. Its data stays here. Leave sync to set it up again.",
  the page shows only Leave sync, and the app menu row says "Sync: device
  removed".
- **Joining again** needs a new cache GUID, since the server refuses the
  old one. Leaving clears the profile's sync transport data (as Reset
  does, `SyncTransportDataPrefs` keyed to the empty Gaia ID): signing out
  of local sync at the next launch wouldn't, so before this a device that
  left and joined again came back with the same GUID. With no cache GUID
  the engine makes a new one at the next join and drops every type's
  metadata. The pref goes on leaving too.
- It isn't a revocation: the device keeps the seed, so it (or anyone with
  the recovery code) can join again. Reset sync locks everyone out.

## Testing

- **Unit tests** in `compass_unittests`, among them: `Bip39Test.*`,
  `ChainTest.*`, `ChainProfilesTest.*`, `PairingCodeTest.*`,
  `PairingKeysTest.*`, `PairingTest.*` (both pairing roles against an
  in-memory server: right and wrong code, deny, expiry, closed reasons,
  server name binding, profiles), `CompassSyncEngineTest.*`,
  `CompassSyncPageHandlerTest.*`, `SyncSettingsTest.*`,
  `CompassHistorySyncTypesTest.*`, `CompassHistoryControllerTest.*`,
  `CompassHistoryDeleteDirectivesTest.*`, `CompassKeptDirectivesTest.*`,
  `CompassSpacesSyncBridgeTest.*`, `CompassSpacesAssignPositionsTest.*`,
  `CompassSpacesSyncProfilesTest.*`, `All/CompassSpacesSyncMoveTest.*`,
  `ProfileMappingTest.*`, `MissingProfilesTest.*`,
  `CompassSyncNotifier*`, `CompassSyncQuotaTest.*`, `QuotaFetcherTest.*`,
  `CommandBarPageHandlerTest.TabsOnOtherDevices`, `OtherDeviceTabsTest.*`,
  `SidebarImportIdsTest.*`. Run only the suites touched.
- **The bridge's tests** run two and three bridges (and two devices × two
  profiles) against an in-memory server with mock time: every apply rule,
  the merge, Today and archive rules, restarts. The fake server doesn't
  send a device a change it superseded itself.
- **No two-client `SyncTest`.** `FakeServer` itself works with route 2
  (`SyncServiceFactory`'s HTTP post provider override reaches
  `CompassEngineComponentsFactory` the same way), but the two-client
  harness (`SyncTest`, `SyncServiceImplHarness`) signs each client in with
  a test Gaia account and waits on states that assume one; local sync has
  none, and upstream only tests it single-client (`LocalSyncTest`). A
  two-client local-sync harness would be its own project; the multi-bridge
  unit tests and the end-to-end helpers stand in for it.
- **Go**: `go vet ./... && go test ./...` in `server/` (pairing, events,
  quota, datastore, command, end to end over HTTP with real tokens,
  encrypted history, backups).
- **End to end**: headless Compass instances driven over the DevTools
  protocol against a local `compass sync-server`, by hand when sync
  changes: [tools/sync_e2e/README.md](../../tools/sync_e2e/README.md).
- **Not covered by automation**: refresh on focus (headless windows don't
  activate), "Show recovery code" (Touch ID), and chrome://history checked
  by eye (its `queryHistory` returns nothing in headless runs, even
  without sync).
- `chrome://sync-internals` stays available for debugging (traffic dump,
  entity browser).
- `CompassSyncService` itself has no unit test (it needs a profile, a sync
  service and os_crypt, and its profile code real profiles); its type
  logic is the tested `sync_settings` functions, and the end-to-end runs
  cover the rest.

## Company sign-in (OIDC)

Optional, off by default (`COMPASS_SYNC_OIDC_ISSUER` unset: nothing
changes). On, a chain syncs only once someone has signed in with the
company's identity provider and the chain is bound to that user. Login
gates access to the server and never touches the keys: the server and the
identity provider still can't read any data. Setup: `server/README.md`,
"Company sign-in (OIDC)".

- **The server is the relying party** (`server/oidclogin`, `coreos/go-oidc`
  and `x/oauth2`): authorization code flow with PKCE (S256), state and
  nonce. The one redirect URI is
  `https://<COMPASS_SYNC_PUBLIC_HOST>/v2/compass/login/callback` (http for
  a localhost host); clients never handle OAuth redirects, so no loopback
  listener or URL-scheme callback is needed.
- **Clients poll, a code stops phishing** (as RFC 8628): the client creates
  a login request with its chain token, shows the short code, opens the
  URL in a browser and long-polls. After the identity provider, the server
  shows the provider, the email, the device name and the same code, and
  asks to Allow. Someone sent an attacker's login link would see a device
  and a code their Compass doesn't show. A cookie set at `/start`
  (HttpOnly, SameSite=Lax) ties the callback and the consent to the
  browser that started; the consent form carries a CSRF token.
- **Per chain, not per device.** A device paired into the chain is signed
  in already; a profile account is another chain, which a signed-in chain
  links (`/account/link`), so there's no second sign-in.
- **The gate.** `auth.AuthorizeSync` is `Authorize` plus "is the chain
  bound, not expired, not revoked, by this issuer?" (cached 10 s), used by
  `/command/`, pairing, events, quota and push. An unbound chain keeps its
  data and gets `401 {"error":"login_required"}` (with
  `X-Compass-Login: required` on `/command/`), never `DISABLED_BY_ADMIN`,
  which makes Chromium stop sync and clear its data. A client tells it
  apart from a refused key by asking `/v2/compass/account`.
  `COMPASS_SYNC_ALLOWED_CHAINS` and `BLOCKED_CLIENT_IDS` still apply.
- **Policy.** `COMPASS_SYNC_OIDC_ALLOWED_DOMAINS` (the email's domain, with
  `email_verified`) and `_ALLOWED_GROUPS` (any of them, in
  `_GROUPS_CLAIM`, default `groups`), checked at sign-in and on each
  refreshed ID token.
- **Revalidation.** With `offline_access`, the user's latest refresh token
  is refreshed every `_REVALIDATE_HOURS` (24): `invalid_grant` or a
  refreshed ID token the policy refuses unbinds all the user's chains;
  network errors and 5xx keep them. Bindings with a refresh token don't
  expire (`expires: 0`); without one they last `_SESSION_DAYS` (30).
- **Storage** (schema 3): `oidc_users` (issuer, subject, email, name,
  created, last login, revoked, refresh token, last checked; unique on
  issuer and subject) and `chain_logins` (chain ID lower-case, user,
  created, session expiry). Pending logins are in memory, 10 min, rate
  limited per chain (10) and per address (30) per 10 min.
- **Admin** (`compass-sync-server`): `users list`, `users revoke
  EMAIL|SUB [--delete-data]` (unbinds every chain of the user and keeps
  them out until `users restore`; `--delete-data` clears and disables those
  chains, as idle chains are), `chains list`.
- **Development:** `COMPASS_SYNC_DEV_FAKE_OIDC=1`, only with a localhost
  public host, serves a fake identity provider at `/dev/fake-idp` (type any
  email and name) and signs in against it; `oidclogin/fakeidp` is the same
  provider the Go tests use.

API (all JSON; chain token = the usual `Authorization: Bearer`):

| Route | Auth | Answer |
|---|---|---|
| `GET /v2/compass/info` | none | `{"login": null}` or `{"login": {"type": "oidc", "name"}}` (`_NAME`, else the issuer's host) |
| `GET /v2/compass/account` | chain token, not gated | `{"login_required", "user": null \| {"name", "email"}, "expires": <Unix ms, 0 = none>}` |
| `DELETE /v2/compass/account` | chain token | `204`, unbinds the chain (every device of it; clients don't call it on Leave) |
| `POST /v2/compass/account/link` | bound chain token, the other chain's token in `X-Compass-Linked-Token` | `204`; the other chain gets the same user; `401 login_required` if the caller isn't bound |
| `POST /v2/compass/login` `{"device_name", "return": "tab" \| "compass"}` | chain token, not gated | `{"id", "url", "user_code": "XXX-XXX", "expires": <Unix ms>}`; `404 {"error":"login_disabled"}` when off, `503 login_unavailable` before the provider is reached, `429` |
| `GET /v2/compass/login/{id}[?wait=0]` | the creating chain's token (another chain: `404`) | long poll ≤ 30 s: `{"status": "pending" \| "done" \| "denied" \| "expired", "user"}` |
| `GET /v2/compass/login/{id}/start` | browser | `302` to the identity provider |
| `GET /v2/compass/login/callback` | browser | the consent page (or an error page) |
| `POST /v2/compass/login/{id}/approve`, `/deny` | browser, cookie and CSRF | the done page, or with `return=compass` `302 compass://sync-login-done?status=done\|denied` |

The pages load nothing external (CSP `default-src 'none'; style-src
'unsafe-inline'; form-action 'self'; frame-ancestors 'none'`, plus
`compass:` in `form-action` for `return=compass`, since browsers check it
on the redirect too).

### Client (desktop and Android)

The C++ is shared; iOS has its own (ios.md).

- **`LoginClient`** (`components/sync/login_client.{h,cc}`, in `:notifier`):
  the requests above with `SimpleURLLoader`, signed with a chain's token,
  and `WaitForLogin()`, long polls one after the other until an answer
  (a network error waits 3 s; a 404 or 401 is "expired"; the login's
  `expires` ends it). Tests: `LoginClientTest`, `LoginClientParseTest`.
- **State** (`CompassSyncService`, `SyncStatus.login`, `SyncState::kLoginRequired`):
  at each start the running chain asks `/info` (once a run) and, if the
  server has sign-in, `/account`. A refusal (`CONNECTION_AUTH_ERROR`, a 401
  from the notifier or the quota) asks again, at most every 30 s; with
  `login_required` the page says "Sign in to <provider> to sync" with a
  button, otherwise the old "server refused this device's key". A signed-in
  chain shows "Signed in as name (email)". Menu: "Sync: sign in needed".
- **Signing in** (`StartLogin()`): `POST /login` with the DEVICE_INFO name
  (else the machine's), the server's page in a new foreground tab (desktop:
  `Navigate()` in the profile's last active window; Android: `SyncBridge`
  opens it from the screen that started it), the code on the page, then
  `WaitForLogin()`. Done: the tab closes if it still shows the server's
  origin, and the engine retries at once. Chromium's scheduler backs off up
  to 10 minutes after 401s and local sync has no credentials to update, so
  `CompassEngineComponentsFactory` wraps `SyncSchedulerImpl` in a
  `RetryableScheduler`, which `SyncCredentials::RetryNow()` reaches on the
  sync sequence to call `OnCredentialsUpdated()` (it retries after an auth
  error). The notifier and quota start over too. `CancelLogin()` ends it;
  calling `StartLogin()` while one runs opens its page again.
- **New chains sign in before they're saved.** `StartNewChain()` and
  `JoinWithRecoveryCode()` ask `/info` with the new seed's token, then
  `/account`; if the server wants sign-in and the chain isn't bound, the
  sign-in runs first (the page shows "This server requires sign-in with
  <provider>" and the code on #off) and the chain is saved, for the
  restart, only once it's done; canceling or failing saves nothing. This
  needs no key in the engine (the token comes from the seed in memory) and
  so doesn't depend on the restart. A chain the server is unreachable for
  is saved as before; after the restart a refusal shows the sign-in.
  Joining by pairing needs none (the chain is signed in). "Reset sync"
  makes an unbound chain: after its restart it asks to sign in.
- **Profiles.** A profile account is its own chain. The home profile links
  each one it enrolls (`POST /account/link` with the profile's token in
  `X-Compass-Linked-Token`), and new profiles made from the chain's list;
  UUIDs whose link didn't get through (network, home not signed in) stay in
  `compass.sync.login_links_pending` and are tried at each start. After the
  home signs in it links every profile of the chain here (revoking unbinds
  them all). A profile account whose `/account` says `login_required` asks
  its home to link it once a run; its "Sign in" button runs the home's
  sign-in. On a server without sign-in the list is just cleared.
- **Leaving doesn't sign out.** Sign-in is the chain's: `DELETE /account`
  would stop every other device of it. The device forgets the seed as
  before; `LoginClient::SignOut()` exists but nothing calls it yet. "Reset
  sync" makes a new chain, which signs in (or is linked) again.
- **UI:** `resources/sidebar/settings/sync_page.{html,ts}` (`#loginRequired`,
  `#loginPanel` with the code, Open the sign-in page and Cancel,
  `#signedInAs`); Android `SyncScreenUi.kt` (android.md, "Sync and push").

## Rooms

Rooms share a space, a pinned folder or an easel between users (chains of
different people), live, with roles. The server stores and orders a room's
records but can't read them: each is encrypted with the room key of an
epoch, and members get the keys as envelopes sealed to their identity keys.
This section is the server's contract (`server/rooms`,
`server/datastore/rooms.go`, schema 5); clients are written against it.
Why rooms and not more sync chains, and how clients use them (crypto,
links, mounts, UI): the collaboration plan.

**Concepts.**

- **Room**: one shared item. `id`: 16 random bytes, chosen by the creating
  client (or by the server when it sends none),
  written everywhere (JSON, paths, events, the admin commands) as base64url
  without padding: 22 characters of `[A-Za-z0-9_-]`, exactly as a link's
  `r`; its bytes are the `room_id` of the record AAD and envelope `info`
  ("Rooms: crypto and links"). IDs are case-sensitive; a path whose ID isn't
  canonical unpadded base64url of 16 bytes is `404 not_found`. `kind`:
  `space`, `folder` or `easel` (a hint
  for clients; the server doesn't use it). The root's UUID, names and every
  other content live in encrypted records, never in the clear.
- **Member**: a chain account (`client_id`, the chain ID: the account's
  Ed25519 public key, 64 lower-case hex digits in answers, any case in
  requests). Roles: `owner` (exactly one), `editor`, `viewer`. A member
  invited by email is **pending** until it joins: it sees the room, its
  members and its envelopes, can set its profile, join or decline (leave),
  and nothing else.
- **Identity key**: an X25519 public key, 32 bytes, per chain; the server
  stores it opaquely (`identity_keys`) and in each membership.
- **Epoch**: the room key's generation, from 1. Records carry the epoch of
  the key that encrypted them; the server accepts writes of the current
  epoch only. A rotation (after a removal) starts the next epoch.
- **Envelope**: a room key of one epoch sealed to one member: exactly 80
  bytes (HPKE `enc` 32, sealed key 32, tag 16; any other size is `400`),
  otherwise opaque to the server.
- **Record**: `id` (1 to 128 of `A-Z a-z 0-9 . _ : -`, chosen by clients,
  unique in the room), `seq`, `epoch`, `deleted`, `author` (the chain that
  wrote it last), `ciphertext` (nonce 12 || ciphertext || tag 16: 28 bytes
  to 1 MiB, else `400` or `413`; none once deleted). Each
  write gets the room's next `seq`, so a record's `seq` is its last
  change's; the latest write wins. Deletes keep a tombstone (in the changes
  feed for good).
- **Invite**: a link invite: `id` (16 random bytes made by the server, as
  unpadded base64url, 22 characters; paths accept 1 to 64 of
  `[A-Za-z0-9_-]`, as a link's `i`), a role (`editor` or `viewer`), an
  optional expiry and number of uses, and `SHA-256(secret)` as 64
  lower-case hex digits. The secret is 32 random bytes the clients keep; it
  goes in the link's fragment (`x`) and reaches the server only when a join
  presents it.
- **Profile**: a member's own blob (at most 4096 bytes), for the
  self-chosen name, encrypted by clients; the server never reads it.

**Conventions.** Every route takes the chain token (`Authorization: Bearer
<token>`, as `/v2/command/`). Bodies and answers are JSON
(`Content-Type: application/json`, `Cache-Control: no-store`); a request
body is one JSON object (anything after it is a `400`); unknown fields are
ignored except on `PATCH .../members/{client}`. Binary fields
(`identity_pub`, `envelope`, `sealed`, `ciphertext`, `profile`) are base64,
standard alphabet with padding (RFC 4648 §4). The exceptions follow the
link's format: room and invite IDs and a join's `secret` are base64url
without padding (RFC 4648 §5), and `secret_hash` is lower-case hex. Times
are Unix milliseconds;
`0` means none. An optional `client_id=<cache GUID>` query parameter names
the calling device: the `room` events its change causes skip that device's
stream, and a removed device is refused (`403 device_removed`). Request
bodies are at most 1 MiB, 24 MiB for `POST .../records`.

**Checks, in order**, on every route: the token (`401 invalid sync token`);
company sign-in, when on (`401 login_required`, as everywhere); a removed
device (`403 device_removed`); a disabled chain (reset or expired, `403
chain_disabled`); the private server's gate (below, `403
chain_not_allowed`); the request's form (`400`); then the room's rules
(roles, epochs, limits), checked in the same database transaction as the
write. A request with a valid token marks its chain active
(`last_active`), as `/v2/command/` does.

**The private server's gate.** With `COMPASS_SYNC_ALLOWED_CHAINS` set, a
chain it doesn't list may still use rooms (its personal sync, pairing,
push, quota and events stay refused): `GET`, `DELETE` and every route under
`/v2/compass/rooms/{room}` of a room it is or was a member of (a removed
member then gets `403 removed`); `POST /rooms/{room}/join` with a valid
invite, or as a pending member; `GET /v2/compass/rooms` and
`GET`/`PUT /v2/compass/identity` while it's a current member of some room.
`POST /v2/compass/rooms` and the directory need a listed chain. Every
other case is `403 {"error":"chain_not_allowed"}`. The token checks
(signature, time, `BLOCKED_CLIENT_IDS`) are the same as everywhere. With
company sign-in, every room route needs a signed-in chain, members
included.

**Errors** are `{"error": "<code>"}`, with more fields where said:

| Status | `error` | When |
|---|---|---|
| 400 | `bad_request` (and `detail`, a sentence for logs) | malformed body or parameter |
| 400 | `invalid_linked_token` | successor: `X-Compass-Linked-Token` missing, bad, expired or blocked |
| 401 | `invalid sync token` | no or bad token |
| 401 | `login_required` | company sign-in on, chain not signed in (`X-Compass-Login: required`) |
| 403 | `device_removed` | the `client_id` was removed from its chain (`X-Compass-Device: removed`) |
| 403 | `chain_disabled` | the chain was reset or expired |
| 403 | `chain_not_allowed` | private server, chain not listed, route not open to it (above) |
| 403 | `removed` | the caller was removed from the room (or left) |
| 403 | `forbidden` | the caller's role doesn't allow it |
| 403 | `not_joined` | the caller is a pending member (join first) |
| 403 | `invite_invalid` | unknown invite, wrong secret, or no such room (a join never tells which) |
| 403 | `invite_expired`, `invite_used_up`, `invite_revoked` | the secret was right, the invite can't be used |
| 404 | `not_found` | no such room, or the caller was never a member |
| 404 | `member_not_found` | the target isn't a current member |
| 404 | `invite_not_found` | no such invite (revoking) |
| 404 | `directory_disabled` | company sign-in is off (directory, email invites) |
| 409 | `conflict` and `current` | a `base_seq` didn't match (see records) |
| 409 | `stale_epoch` | an epoch other than the one expected; read the room again |
| 409 | `envelopes_mismatch`, `missing`, `unknown` | a rotation's envelopes don't match the members |
| 409 | `owner_must_transfer` | the owner tried to leave or be removed |
| 409 | `room_full` | 50 members (joined and pending) |
| 409 | `too_many_rooms` | the chain owns 1000 rooms |
| 409 | `room_exists` | creating a room with an ID already taken |
| 409 | `too_many_invites` | 100 invites not revoked |
| 409 | `already_member` | email invite, or successor, of a current member |
| 409 | `successor_disabled` | successor: the new chain was reset or expired |
| 409 | `no_identity_key`, `identity_mismatch` | email invite: the account has no key, or another one than sent; joining: another key than the email invite's |
| 409 | `not_signed_in` and `client_id` | email invite of an account not signed in |
| 413 | `record_too_large` | a record's ciphertext over 1 MiB |
| 413 | `too_large` | the body, or a write's ciphertext in all (16 MiB) |
| 507 | `quota_exceeded` | the owner's chain is full (`COMPASS_SYNC_MAX_BYTES`) |
| 503 | `login check unavailable` | the sign-in gate couldn't tell |
| 500 | `unable to complete request` | |

**Objects in answers.**

Room (`GET /rooms/{room}` and the routes that answer a room):

```json
{
  "id": "n4bQgYhMfWWaL-qgxVrQFQ",
  "kind": "folder",
  "owner": "<chain id>",
  "created": 1793491200000,
  "epoch": 2,
  "seq": 42,
  "bytes": 18234,
  "role": "editor",
  "pending": false,
  "members": [Member, ...],
  "envelopes": [{"epoch": 1, "sealed": "<base64>"}, {"epoch": 2, "sealed": "<base64>"}],
  "invites": [Invite, ...]
}
```

`role` and `pending` are the caller's. `seq` is the latest record's (0: no
record yet). `bytes` counts the records' ciphertext and IDs, against the
owner's quota. `members`: the current members, joined and pending, by
`client_id`. `envelopes`: the caller's own, by epoch (other members' are
never shown). `invites`: only in the owner's answers (absent for others),
oldest first, revoked and used-up ones included.

Member:

```json
{"client_id": "<chain id>", "role": "viewer", "identity_pub": "<base64, 32 bytes>",
 "profile": "<base64>" | null, "name": "Ada Lovelace", "email": "ada@example.com",
 "invited": 1793491200000, "joined": 1793491260000, "pending": false}
```

`name` and `email` come from company sign-in (the user the chain is bound
to); `""` without sign-in or binding. `joined` is `0` while `pending`.

Invite: `{"id", "role", "created", "expires" (0 = never), "uses_left"
(null = unlimited), "revoked"}`. The secret's hash is never answered.

Record:

```json
{"id": "node:3f2a…", "seq": 42, "epoch": 2, "deleted": false,
 "author": "<chain id>", "ciphertext": "<base64>"}
```

`ciphertext` is absent when `deleted` is true.

**Routes** (all under `/v2/compass`):

`GET /rooms`. Any account (on a private server: a member of some room).
`200 {"rooms": [{"id", "kind", "owner", "role", "pending", "epoch",
"seq"}]}`, the caller's current memberships, pending included, by `id`. A
client calls it when a `room` event names a room it doesn't know (an email
invite) and at start.

`POST /rooms` `{"id"?, "kind", "identity_pub", "envelope", "profile"?}`.
Any listed account; the caller becomes the owner. `id`, optional: the room
ID the client chose (16 random bytes, canonical unpadded base64url, `400`
otherwise), so it can seal the first envelope with the room ID in its HPKE
`info` before the room exists; absent or `""`, the server picks one (then
the client must seal for an ID it doesn't know yet, so clients send it).
`identity_pub`: its X25519 public key (32 bytes, also stored as its
identity key); `envelope`: the epoch 1 key sealed to itself, for this
room's ID. `201` Room (epoch 1, seq 0, no invites). `409 room_exists` if
the ID is taken (pick another), `409 too_many_rooms`.

`GET /rooms/{room}`. Any member, pending too. `200` Room. `403 removed`,
`404 not_found`.

`DELETE /rooms/{room}`. Owner. `204`. Everything of the room goes (records,
envelopes, invites, memberships); its bytes go back to the owner's quota.
Members get a `room` event with `"deleted"`, and `404` afterwards.

`GET /rooms/{room}/changes?since=<seq>&limit=<n>&wait=<seconds>`. Joined
member. `since` default 0, `limit` 1 to 1000 (default 1000), `wait` 0 to
30 (default 0). `200 {"seq": <room's latest>, "epoch": <current>,
"records": [Record, ...], "more": <bool>}`: the records whose `seq` is
greater than `since`, by `seq`, each in its latest state (a record changed
twice comes once). A page also ends after about 8 MiB of ciphertext (at
least one record); with `more` true, ask again with `since` = the last
record's `seq`. With `wait` and nothing new, the request is held until a
record newer than `since` is written (then it answers at once), the
caller stops being a member (`403 removed`, `404 not_found` if the room
was deleted) or the wait ends (then `"records": []`). Other changes
(members, keys) don't end it: they come as `room` events. Start from `since=0` (or the `seq` the client
kept) and keep the last `seq` seen.

`POST /rooms/{room}/records` `{"records": [{"id", "epoch", "ciphertext",
"deleted"?, "base_seq"?}, ...]}`. Editor or owner. 1 to 500 records, each
`id` once. `epoch` must be the room's current one (`409 stale_epoch`
otherwise: read the room, decrypt the new envelope, re-encrypt).
`ciphertext` (28 bytes to 1 MiB) is required unless `deleted` is true
(then it's dropped).
`base_seq`, optional: the `seq` the client based the write on; `0` means
"must not exist" (a tombstone exists). All or nothing: if any `base_seq`
doesn't match, nothing is written and the answer is `409 {"error":
"conflict", "current": [Record, ...]}` with the stored state of each
mismatched record (`seq` 0 and no ciphertext: it doesn't exist); merge and
write again. Without `base_seq` the write wins. `200 {"seq": <room's
latest>, "records": [{"id", "seq"}, ...]}` in request order. The room's
bytes (ciphertext plus ID length) count against the owner chain's
`COMPASS_SYNC_MAX_BYTES` (`507 quota_exceeded` when a write would grow it
past; shrinking always works). `413 record_too_large`.

`POST /rooms/{room}/invites` `{"role": "editor" | "viewer", "secret_hash":
<SHA-256 of the 32-byte secret, 64 lower-case hex digits>, "expires"?: <ms, 0 = never,
else in the future>, "uses"?: <1 to 1000, 0 = unlimited>}`. Owner. `201`
Invite. `409 too_many_invites`.

`DELETE /rooms/{room}/invites/{invite}`. Owner. `204` (again: `204`).
The invite stays listed as `revoked`. `404 invite_not_found`.

`POST /rooms/{room}/join` `{"invite_id"?, "secret"?, "identity_pub",
"profile"?}`. `secret`: the invite's secret itself, 32 bytes as unpadded
base64url (the link's `x` as is); the server compares its SHA-256 with the
stored one in constant time. A malformed `invite_id` or `secret` is `403
invite_invalid`. `200` Room.

- A current joined member: nothing changes (no invite needed, none used).
- A pending member (email invite): joins without an invite; `identity_pub`
  must be the key it was invited with (`409 identity_mismatch`).
- Anyone else needs a valid invite and becomes a member with its role,
  using one use. `403 invite_invalid` for an unknown invite or room or a
  wrong secret, else `invite_revoked`, `invite_expired` (at or after
  `expires`), `invite_used_up`; `409 room_full`.
- A member who was removed (or left) needs an invite created after that
  (`403 removed` otherwise), so an old link doesn't bring it back.

A link invite carries the key of the epoch it was made in; a member who
joins after a rotation needs the newer keys from the owner (`POST
.../envelopes`). The join's `room` event (`"members"`) tells the owner's
devices.

`POST /rooms/{room}/members` `{"members": [{"client_id", "role",
"identity_pub", "envelopes": [{"epoch", "sealed"}, ...]}, ...]}`. Owner,
company sign-in servers only (`404 directory_disabled` otherwise). The
email invite: 1 to 50 accounts, found with the directory. Each must be
signed in (`409 not_signed_in` with its `client_id`), have registered
`identity_pub` as its identity key (`409 no_identity_key`,
`identity_mismatch`: look it up again), not be a current member
(`409 already_member`), and come with an envelope for the current epoch
(earlier epochs optional; an epoch outside 1 to current is `409
stale_epoch`). They're added pending. All or nothing. `200` Room. Each
invitee's chain gets a `room` event; it calls `GET /rooms`, then joins or
declines.

`PATCH /rooms/{room}/members/{client}` `{"role"?: "editor" | "viewer",
"profile"?: <base64> | null}`. At least one; no other fields. `role`:
owner only, on a member other than itself (`403 forbidden` for the owner's
own role: use the transfer). `profile`: only on oneself (pending members
too); null or `""` clears it. `204`.

`DELETE /rooms/{room}/members/{client}`. The owner, or the member itself
(leaving; a pending member declining). The owner can't leave or be removed
(`409 owner_must_transfer`). `204`. The member's envelopes are deleted;
its row stays, marked removed (`403 removed` from then on, also on its open
long polls). After removing someone the owner's client rotates the key.

`POST /rooms/{room}/owner` `{"client_id"}`. Owner. The target must be a
joined member (`404 member_not_found`, `403 not_joined`). It becomes the
owner, the old owner an editor; the room's bytes move to the new owner's
quota (`507 quota_exceeded` if they don't fit). `204`. With an
`X-Compass-Linked-Token` header, the same route is the successor form
below (any member, body as below).

`POST /rooms/{room}/successor`, header `X-Compass-Linked-Token: <a token
of the new chain>`, body `{"identity_pub": <the new chain's X25519 key,
32 bytes>, "envelopes": [{"epoch", "sealed"}, ...]}`. Any member, pending
too, called by the old chain (Bearer). For "Reset sync": before the old
chain is cleared, the client moves each of its memberships to the
account's new chain, so a reset doesn't lose the rooms. The linked token
proves control of the new chain (signature and time checked,
`BLOCKED_CLIENT_IDS` applies, the private server's list doesn't; `400
invalid_linked_token`; the caller's own chain is a `400`). `envelopes`:
the room keys sealed to the new chain's key, 1 to 1000, one per epoch,
each 80 bytes, epochs 1 to current, the current one required (`409
stale_epoch`); seal every epoch the client holds a key for, so the new
chain reads the older records too. The new chain becomes a member with
the caller's role, profile and pending state (the owner's successor owns
the room, and the room's bytes move to its quota: `507 quota_exceeded`),
its identity key is recorded, and the old chain is marked removed (`403
removed` from then on). It needs no invite and, on a private server, no
place in the list for this room (as any member, "The private server's
gate"); with company sign-in it needs to be signed in to use the room, as
always. `409 already_member` if the new chain is a current member, `409
successor_disabled` if it was reset or expired. `204`; members, the old
chain included, get a `"members"` event. No key rotation is needed: the
same account holds both chains.

`POST /rooms/{room}/epochs` `{"epoch": <current + 1>, "envelopes":
[{"client_id", "sealed"}, ...]}`. Owner. Starts the next epoch: exactly one
envelope for every current member, joined and pending, the owner included.
`409 stale_epoch` if `epoch` isn't current + 1; `409 {"error":
"envelopes_mismatch", "missing": [<client ids without one>], "unknown":
[<client ids that aren't members>]}` (a member joined or left meanwhile:
read the room and retry). `204`. Writes must use the new epoch from then
on; older records stay readable with older keys.

`POST /rooms/{room}/envelopes` `{"envelopes": [{"client_id", "epoch",
"sealed"}, ...]}`. Owner. 1 to 1000 envelopes of existing epochs (1 to
current; `409 stale_epoch` beyond) for current members (`404
member_not_found`); an existing one of the same member and epoch is
replaced. For members who joined by a link made before a rotation. `204`.

`GET /identity`. `200 {"identity_pub": <base64> | null}`, the caller's.
`PUT /identity` `{"identity_pub"}` (32 bytes). `204`. Registers or
replaces the caller's identity key (and in its current memberships; the
rooms' members get a `"members"` event). Clients register it on sign-in
(company servers, so the directory finds them) and before joining.

`GET /directory?email=<address>`. Company sign-in servers only (`404
directory_disabled` otherwise); listed, signed-in callers. `200
{"accounts": [{"client_id", "identity_pub", "name", "email"}, ...]}`: the
chains bound to the users of this server's issuer whose email is that
address (case-insensitive), not revoked, session not expired, with an
identity key; by `client_id`, empty if none. A user's profiles are separate
chains: each is an account to invite.

**Notifications.** Every change sends a `room` event on the change stream
(`/v2/compass/events`, "Change notifications") of every current member's
chain (and of a removed member, for its removal), except the stream of the
device that made it (its `client_id`):

```
event: room
data: {"room_id":"n4bQgYhMfWWaL-qgxVrQFQ","seq":42,"changes":["records"]}
```

`changes` (sorted): `records` (a write), `members` (join, email invite,
role, profile, removal, leave, transfer, identity key), `keys` (rotation;
envelopes added, to their members only), `invites` (to the owner only),
`deleted`. Changes of one room merge while the stream is busy; `seq` is the
room's latest. Clients that predate rooms ignore it: `CompassSyncNotifier`
and iOS's `SyncNotifier` act on `event: changed` only. No push (APNs,
FCM): rooms aren't urgent. An admin's `rooms delete` (another process)
sends nothing; members find out at their next request.

**Limits.** 50 members (joined and pending), 1 MiB ciphertext per record,
500 records and 16 MiB of ciphertext per write, 1000 records or about 8
MiB per changes page, 100 open invites per room, 1000 rooms owned per
chain, long polls of 30 s at most, envelopes exactly 80 bytes, records
28 bytes or more, identity keys 32, profiles at most 4096.
Room bytes count against the owner chain's `COMPASS_SYNC_MAX_BYTES` (its
row in `chains`, found case-insensitively; `GET /v2/compass/quota` shows
them in `bytes`).

**Going away.** Rooms follow their chains. On "Reset sync" clients first
move the old chain's memberships to the new chain (`POST
.../successor`), so its rooms survive. When a chain's data is cleared
(reset sync, `COMPASS_SYNC_IDLE_CHAIN_DAYS`, `users revoke --delete-data`),
every room it still owns is deleted, its other memberships, envelopes and
identity key are deleted (the member just disappears from the room;
clients should rotate), and its room bytes go. No events are sent then.

**Storage** (schema 5): `rooms` (id, owner lower-case, the `chains` row
its bytes count in, kind, created, seq, epoch, bytes), `room_members`
(room, chain lower-case, role, identity key, profile, invited, joined (0:
pending), removed (0: current)), `room_records` (room, record ID, seq,
epoch, deleted, author, ciphertext, size; indexed by room and seq),
`room_envelopes` (room, chain, epoch, sealed), `room_invites` (room, id,
SHA-256 of the secret, role, created, expires, uses left (NULL:
unlimited), revoked), `identity_keys` (chain, key, updated). Room tables
cascade from `rooms`. Nothing in them is readable without the room keys,
which the server never sees.

**Admin** (`compass-sync-server rooms ...`, README "Rooms"): `list` (ID,
kind, owner, members, epoch, seq, bytes, created), `show ROOM` (members
with role and status, removed ones too, and invites; never contents),
`delete ROOM`.

## Decisions

1. **Server**: self-hostable; the project runs a public instance too, but
   the desktop app has no default server.
2. **What syncs**: everything that can, Today and the archive included.
3. **First sync on a device with its own spaces**: merge.
4. **No accounts by default; an optional OIDC gate, keys untouched**: a
   sync chain. Devices join with a short pairing code from a device already
   syncing; a 12-word recovery code is the fallback. A company server can
   require sign-in (see "Company sign-in (OIDC)").
5. **Route 2** (local-sync path) over a synthesized account.
6. **Passphrase per profile account**, for domain separation.
7. **Old chains expire on the server** rather than being deleted at reset.

## Known limitations

- **No revocation**: a stolen Mac with the seed can read new data until
  "Reset sync".
- A remote delete of the last space leaves it (and the next pass puts it
  back).
- An archived split built from a remote entity whose panes haven't all
  come shows the panes it has; restoring it before the rest arrive restores
  those.
- Today dedupe by URL is only at the first sync.
- Partial delivery of a large folder delete (over more than one commit
  message) can rescue some children to the root before their tombstones
  arrive.
- A profile with no space in the switcher isn't listed, so not kept loaded:
  its sync runs only while something else loads it. Moving a profile's last
  space away unlists it on both devices (each adds a fresh "Home" to the
  unlisted profile, which its account then carries).
- Names follow the chain, so mapping "Job" to the chain's "Work" renames
  it "Work" here.
- Deleting a profile stays on its device: other devices keep theirs, and
  the chain keeps listing it. A device that never had it still creates it.
- Two devices that both hold the home role for a moment (a race at join)
  converge on one home UUID; spaces get re-committed once with the new
  `profile_uuid`.
- Leave and Reset act on the profiles loaded at the time. A chain profile
  that isn't loaded then keeps the old seed and rejoins the old chain the
  next time it loads.
- Moving a space to another profile finishes once that profile's
  `SpaceService` has loaded; quitting within a second or two drops the move
  (the space stays in the old profile, nothing lost).
- The history-full warning appears at the next quota check (up to 30 min,
  or at the next start), since dropped visits don't fail a commit.
- A device that comes back to an expired chain is told only "sync stopped"
  in upstream's words.
- The throttled message can't tell 50 devices from other throttling.
- Device times ("9 minutes ago") follow the system locale while the page's
  own strings are English only.
- Change notifications and pairing slots live in one server process's
  memory.

## Open questions

- A "deleted" flag on `CompassProfile`, if profiles should be deletable
  everywhere.
- The home re-enrolling chain profiles whose seed differs from its own
  (the Leave/Reset limitation above).
- Finishing a pending space move at shutdown, or keeping it in Local State
  and redoing it at the next start.
- Brave's priority swap for history delete directives, gated so it doesn't
  change non-sync profiles.
- A Compass message for `DISABLED_BY_ADMIN`.
- Field-level merge for spaces entities, if last-writer-wins on a whole
  entity bites.

## Risks

- **Rebase cost**: the data-type patches touch tables that upstream edits
  often (`data_type.h`, the histogram enum). Small but frequent conflicts.
- **Upstream moving away from sync-the-feature** on desktop (transport mode
  + account stores). Route 2 depends on local sync staying; Brave's tree is
  the reference for how they keep up.
- **Protocol drift**: newer Chromium may send fields go-sync doesn't know.
  The server stores specifics opaquely and only interprets envelope fields.

## Rooms: crypto and links

The cryptography every client shares for rooms (shared spaces, folders and
easels), and the invite link. The desktop and Android use
`components/sync/rooms/` (`//compass/components/sync/rooms:crypto`); iOS uses
CompassKit's `CompassRooms` (`RoomCrypto.swift`, `RoomInviteLink.swift`).
Both test the same vectors, `common/rooms/vectors.json`, which
`common/rooms/make_vectors.swift` writes with its own RFC 9180 code on
CryptoKit primitives (so neither client checks only itself). In the
formulas, `||` is byte concatenation and `epoch` is a big-endian `uint32`.

**Identity key.** Each server account has an X25519 key pair derived from
the chain seed, in the style of the auth key:

    private = HKDF-SHA256(ikm = seed, salt = "", info = "compass-identity-x25519"
                          [":" + profile_uuid], L = 32)
    public  = X25519(private, 9)

An empty profile UUID gives the chain's home account's key; a profile
account uses its sync UUID, as for `compass-sync-auth`. The private key is
used as the scalar; X25519 clamps it. Members publish `public` when they join
a room.

**IDs.** A room ID is 16 random bytes (base64url without padding in links;
the server's encoding is its own). A record ID is the entity's UUID string
(space, node, easel, easel object or asset), as in `CompassSpacesSpecifics`.

**Room keys and envelopes.** A room key is 32 random bytes per epoch. It
reaches each member as an envelope, HPKE (RFC 9180) base mode with
DHKEM(X25519, HKDF-SHA256), HKDF-SHA256 and AES-256-GCM (suite
0x0020/0x0001/0x0002; BoringSSL `EVP_HPKE_*`, CryptoKit
`HPKE.Ciphersuite(kem: .Curve25519_HKDF_SHA256, kdf: .HKDF_SHA256,
aead: .AES_GCM_256)`, since CryptoKit has no preset for it):

    info     = "compass-room-key" || room_id || epoch
    envelope = enc (32) || Seal(room_key, aad = "")       -- 80 bytes

The room and epoch are bound by `info`, so an envelope can't be replayed into
another room or epoch. Each seal uses a fresh ephemeral key, so the vectors
hold envelopes with a fixed ephemeral key (BoringSSL's
`EVP_HPKE_CTX_setup_sender_with_seed_for_testing` reproduces them byte for
byte; iOS opens them) and envelopes CryptoKit sealed (both open them).

**Records.** A record's content (a serialized `CompassSpacesSpecifics`; bytes
at this layer) is AES-256-GCM under the epoch's room key:

    aad    = "compass-room-record" || room_id || record_id (UTF-8) || epoch
    record = nonce (12, random) || ciphertext || tag (16)

The associated data stops the server moving a record to another room, record
ID or epoch. Fixed nonces exist for the vectors only
(`EncryptRecordWithNonceForTesting`, the Swift internal `nonce:` overload).

**Invite links.**

    https://compassbrowser.org/join#<base64url, no padding, of the JSON>
    compass://join#<same>                       (accepted when reading)

The JSON is compact with sorted keys, and strings escape only `"`, `\` and
control characters (UTF-8 otherwise), so every client writes the same bytes:

    {"e":epoch,"i":invite_id,"k":b64url(epoch_key),"n":name,
     "r":b64url(room_id),"s":server_url,"t":"space"|"folder"|"easel",
     "x":b64url(invite_secret)}

A reader refuses the link unless all of these hold, and ignores unknown keys
(for later versions):

- The whole link is at most 4096 bytes, starts with one of the two prefixes,
  and the fragment is canonical unpadded base64url of a JSON object.
- `r` decodes to 16 bytes, `x` and `k` to 32 (canonical base64url, no
  padding).
- `e` is a whole number from 0 to 2^32 - 1 (not a string or boolean).
- `i` is 1 to 64 of `[A-Za-z0-9_-]`.
- `n` is a string of at most 256 UTF-8 bytes (it may be empty).
- `t` is `space`, `folder` or `easel`, lowercase.
- `s` starts with `https://`, or `http://` with the host `localhost`,
  `127.0.0.1` or `[::1]` exactly; uses only the characters
  `A-Za-z0-9-._~:/[]!$&'()*+,;=%` (so no user info, query, fragment, spaces
  or backslashes); has a non-empty host, and a port of 1 to 65535 if any.
  The host and port are read from the text itself, then the platform's URL
  parser must accept it too.

The fragment never reaches a server. The invite secret is 32 random bytes;
the server stores only `SHA-256(secret)` as lowercase hex
(`InviteSecretHash()` / `RoomCrypto.inviteSecretHash`) and the joiner proves
the invite by sending the secret. Anyone with the link can read the room at
the link's epoch until the key rotates: revoking a link stops new joins, and
only a rotation stops someone holding it from reading later records.

## Rooms: clients

How the desktop and Android use rooms (C++, shared by both); iOS's twin is
ios.md, "Rooms: sharing with other people", and both write the same
records, mounts, profiles and links.

**Code.**

- `components/sync/rooms/room_client.{h,cc}` (`:client`): `RoomClient`,
  every route of "Rooms" over `SimpleURLLoader` (traffic annotation
  `compass_rooms`), signed with the account's chain token, `client_id` = the
  device's cache GUID. Answers parse into `RoomInfo`, `RoomChanges`,
  `CommitResult`…; failures into `RoomError` (`status` 0 for the network;
  `is_gone()` for `403 removed`/`404 not_found`, `is_conflict()` with
  `current`, `is_stale_epoch()`, `is_forbidden()`, `is_too_large()`,
  `is_quota_exceeded()`, `is_login_required()`, `is_chain_not_allowed()`).
  `MoveToSuccessor()` is Reset sync's `POST .../successor` with
  `X-Compass-Linked-Token`.
- `browser/rooms/room_model.{h,cc}` (`:sync`): `RoomModel`, the shared item
  as records and applying them, scoped to it: `CreateFolderRoomModel()`,
  `CreateSpaceRoomModel()`, `CreateEaselRoomModel()` over the profile's
  `SpaceList` and an `EaselSource` (the `EaselStore`). Node and theme
  conversions are the bridge's (`compass_spaces_specifics_util.h`:
  `FillNodeContent()`, `ApplyNodeContent()`, `ThemeToProto()`…, defined in
  `compass_spaces_sync_bridge.cc`), positions are
  `internal::AssignPositions()`, so a room's records are the chain's wire
  format.
- `browser/rooms/room_sync.{h,cc}` (`:sync`): `RoomSync`, one per mounted
  room, over a `RoomServer` (the client, or a fake in tests).
- `browser/rooms/room_service.{h,cc}` (`:rooms`): `RoomService`, a
  `KeyedService` per regular profile (none for Incognito or Guest), desktop
  and Android; created by the profile's COMPASS_SPACES holder
  (`compass_spaces_sync.cc`, `compass_spaces_sync_android.cc`).
- `browser/rooms/room_link_interceptor.{h,cc}` (desktop), the share sheet
  and join page (`browser/ui/webui/share/`, `chrome://compass-share`,
  `resources/sidebar/share/`), Android's `RoomsBridge`
  (`browser/android/rooms_bridge.*`, `RoomsBridge.java`, `ShareSheet*.{java,kt}`,
  `JoinRoomActivity`).

**The account and its identity.** The member is the profile's sync account:
the chain's own for the home profile, the profile's for a profile account
(`compass.sync.profile_account`, `compass.sync.profile_uuid`).
`RoomService` reads the seed itself (`compass.sync.seed` through
`os_crypt_async`, as `CompassSyncService::LoadSeed()`), derives the auth key
(`DeriveAuthKey(seed, uuid)`, tokens with `MakeAccessToken()`) and the
identity key (`DeriveIdentityKey(seed, uuid)`), and registers the public key
once per server (`PUT /identity`, remembered in the rooms file). Rooms stop
when the seed pref is cleared (Leave).

**Mounts** (`room_mount = 11`, `CompassRoomMount`: `room_id` 1,
`server_url` 2 (the sync server's URL, path included, as in links' `s`),
`kind` 3, `root_uuid` 4, `role` 5, `keys` 6 (`CompassRoomKey {epoch 1, key
2}`), `space_uuid` 7, `parent_uuid` 8, `position` 9, `name` 10). Client tag
`RoomMountKey(room_id)`. The bridge's delegate carries them
(`GetLocalRoomMounts()`, `ApplyRoomMounts()`, applied before anything else
of a remote batch), so every device of the account mounts its rooms;
`RoomService` keeps them, and each room's state, in
`<profile>/Compass Rooms.json` (the mount, the cursor, every record's seq
and plaintext, the members the owner sent keys to, the removed notices).
Moving a shared folder updates its mount's placement.

**Entity kinds an older version refused.** A version that doesn't know an
entity kind refuses it (`IsEntityDataValid()`), and sync counts it as
received, so the server never sends it again: a space joined on the phone
while the Mac ran a build from before rooms never showed on the Mac, even
after it updated (2026-11-21). The bridge's store keeps a note of the kinds
the version that last downloaded the type took (`compass-entity-kinds`, not
an entity: the key isn't a UUID; `kEntityKinds`, 1 = room mounts). A start
that finds the initial sync done and an older note (or none) clears the
progress marker, in the same write as the new note, so the next GetUpdates
asks for everything: what the device has comes back at known versions and
is skipped as reflections, and only what it refused applies, as ordinary
remote changes (no merge, so nothing deleted elsewhere comes back). An
initial merge writes the note. Bump `kEntityKinds` with each new kind.
iOS's bridge skips unknown kinds too, after its engine has taken their
versions, so clearing the token alone wouldn't bring them back there:
`SyncEngine.ensureEntityKinds(.compassSpaces, CompassSpacesBridge.entityKinds)`
(at each account's start, before the bridge is registered) uses `refetch`,
which also forgets the entities' versions (local changes not yet committed
stay), with the note in `TypeState.entityKinds`, saved in the same write.
The initial sync stays done, so it isn't a merge either. Bump both numbers
together.

**What the chain leaves out** (`Delegate::GetRoomRoots()` → `RoomRoots`):
shared spaces (their entity and Pinned; Today and the archive stay in the
chain, keyed under the space's UUID, and wait in the mirror until a room
brings the space: `ApplyWaitingNodes()`), shared folders and their
subtrees, shared easels (boards, objects, and images only they show), the
boards of easel tabs inside shared folders and spaces, and a joined easel's
pinned tab (`RoomTabKey(room_id)`, never synced through the chain).
`ComputeLocalSpecifics()` skips them; remote puts of them are kept in the
mirror but not applied, and remote deletions of them are dropped (another
device of the account shared them). Unlike iOS, the desktop doesn't delete
what the chain had of a shared item: it stays frozen there (a deleted
entity can't come back under its ID), and comes back to life if the room
goes. Each side ignores the other's handling, so mixed devices agree.

**Records.** A record is a serialized `CompassSpacesSpecifics` under the
entity's UUID, sealed per epoch (`EncryptRecord()`). Folder room: `node`s,
section PINNED, no `space_uuid`; the folder itself has no `parent_uuid` or
`position`. Space room: one `space` (uuid, name, theme only) and its
Pinned's nodes with its `space_uuid`. Easel room: the `easel`, its
`easel_object`s and the `easel_asset`s they show, with bytes. Last-active
and archive times never go into a room. A joiner finds the root among the
records (the folder without a parent, the space, the board); links don't
carry it. An item shared on its own inside another shared item (a
joined folder placed in a shared space) belongs to its own room only: the
outer room's model leaves it and what's in it out of its records, never
applies records of it, and doesn't count it for view-only
(`RoomModel::set_excluded()`, every other room's root and joined-easel tab).

**RoomSync's cycle.** `GET /rooms/{id}` (role, epoch, envelopes opened with
the identity key into the mount's keys), then the changes feed as a long
poll (`wait=25`, `since` = the cursor): each record past what's known is
decrypted with its epoch's key (a missing key reads the room again), the
mirror updated, and `RoomModel::Apply()` run with what changed and went
(create in the container, content, then placement parents first by
position, then deletions, then splits fixed). Local changes (`SpaceModel`
and `EaselStore` observers in `RoomService`) schedule a pass 800 ms later:
`GetLocal()` diffed with the mirror, writes of the current epoch with
`base_seq` (the mirror's seq, 0 for a new record), up to 200 records or
8 MiB a request. `409 conflict`: the stored records win (into the mirror
and the model), the rest goes again; `409 stale_epoch` or `403 forbidden`
read the room again; `413`/`507` are reported and not retried until the
next change; network errors back off (2 s to 5 min). Our own writes come
back in the feed with seqs we already have and are skipped. The room is
read again every minute (members, roles, keys); `event: room` isn't
followed yet.

**Viewers** write nothing: a pass with local changes applies the mirror
back instead (`Revert()`). The sidebar's handler refuses edits of a
viewer's shared items (`IsViewOnly()`, `IsViewOnlySpace()`: move, add,
rename, delete, collapse, split, the menu's commands), the easel page's
handler refuses changes and its editor shows the board read-only
(`Easel.view_only`).

**A shared item deleted here** comes back from the room (as on iOS); the
share sheet has Leave and Delete for Everyone.

**Sharing** (`RoomService::Share()`): a room key, a room ID chosen here
(`GenerateRoomId()`; `409 room_exists` tries another), the epoch 1
envelope sealed to the account's identity for that ID, the member profile;
`POST /rooms` with `id`. A server that answers another ID gets the
envelope and profile again for it (`POST .../envelopes`, `PATCH
.../members/{me}`). The mount (role owner, key 1) follows, and the first
pass writes the item's records.

**Invites.** `CreateInviteLink()`: a secret, its hash to `POST .../invites`
(role, expiry, uses), and `EncodeInviteLink()` with the current epoch's key
and the item's name. Joining (`Join()`): `POST .../join` with the secret,
the identity key and the profile; the mount gets the link's key and any
envelopes; the root is found in the first records; the item goes in the
chosen space (folder, easel tab), else the account's first space. Email
invites (company sign-in): `GET /directory`, then `POST .../members` with
every epoch's key sealed to each account. The owner sends every key to
members it hasn't yet (`POST .../envelopes`; links made before a rotation).

**Members.** Change role (`PATCH`), remove (`DELETE`, then a rotation:
`POST .../epochs` with the new key sealed to every remaining member),
leave (`DELETE` of oneself; the owner gets `owner_must_transfer` and is
offered Delete for Everyone), delete (`DELETE /rooms/{id}`). Leaving,
deleting, or `403 removed`/`404 not_found` from the feed unmount the room;
the item stays as an ordinary one and syncs in the chain again. A removal
leaves a notice: a banner above the sidebar until dismissed.

**Names.** A member's `profile` is epoch (uint32, big-endian) ||
`EncryptRecord(key, room_id, "profile:" + chain ID, epoch, {"name": …})`;
company sign-in's name and email win. "Your name for sharing"
(`compass.rooms.display_name`, in the share sheet) defaults to the
computer's user's full name (`NSFullUserName()`, `GetUserNameExW()`, the
GECOS name; Android: none).

**Reset sync.** `CompassSyncService::ResetChain()` makes the new seed first
and calls `RoomService::MoveToNewChain()` for each of the device's profiles
in the chain, before they stop: `POST .../successor` for every mount, with
the old account's token, the new one's as `X-Compass-Linked-Token`, its
identity key, and every epoch's key sealed to it. The mounts then go up to
the new chain with the rest.

**UI (desktop).** Share… in a pinned folder's context menu, Share Easel… in
an easel tab's, Share space… in the space menu: `chrome://compass-share/
?kind=&id=` in a small popup window (Start sharing; Invite with a link with
role, expiry and uses, Copy invite link; Invite by email on company sign-in
servers; People with role menus and Remove; open invite links with Revoke;
Your name for sharing; Leave or Delete for everyone). A people badge with
the members in its tooltip on shared items (`Node.shared`,
`shared_tooltip`, `view_only`; `Space.shared`, `view_only`, `can_share`).
`RoomLinkInterceptor` sends a tab opening `https://compassbrowser.org/join#…`
or `compass://join#…` to `chrome://compass-share/#join=<fragment>` (the
website never loads), whose page shows what's shared and on which server,
asks for the space, and joins.

**UI (Android).** The same through `RoomsBridge`: Share… in a pinned
folder's menu, Share Easel… in an easel tab's, Share Space… in the space's ⋯
menu; the Compose share sheet and join screen (`ShareSheetUi.kt`); a badge on
shared rows and the removed banners in the sidebar. Invite links come
through `JoinRoomActivity` (intent filters for
`https://compassbrowser.org/join`, not autoVerify, and `compass://join`).

**Tests.** `RoomClientTest` (requests, encodings, errors, the successor's
header), `RoomSyncTest` (a folder shared to a member through a fake server,
edits both ways, deletes and moves, a viewer's edits undone and nothing
written, conflicts, the state file), `RoomLinkInterceptorTest`,
`CompassSpacesSyncBridgeTest` for the mounts and exclusions.

**Not yet.** `event: room` (long polls and a refresh every minute instead);
pending email invitations found with `GET /rooms`; ownership transfer in the
UI; a mount's position among siblings (it goes at the end).
