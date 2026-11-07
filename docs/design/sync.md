# Sync between devices

Based on Chromium 156.0.8078.48.

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
- **Removing a device** deletes its `DEVICE_INFO` entry; it can't revoke
  the device, because every device holds the seed. "Reset sync" makes a new
  chain (new seed), uploads this device's data, and the others must join
  again. Same as Brave.

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

(EntitySpecifics field numbers, sorted; never contents.) The controller
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
`CompassSpacesSpecifics` has a oneof of four entities:

- `CompassSpace` (1): one per space. UUID, name, color, icon,
  `directory_position`, `profile_uuid`, `archive_after_seconds`, and the
  theme fields (see "Space themes").
- `CompassNode` (2): one per sidebar node. UUID, `space_uuid` (empty for
  favorites), `parent_uuid` (empty = the root of its section), `section`,
  `position`, `type`, URL, title, custom title, `collapsed`,
  `last_active_time`, `archived_time`, and `saved_url`/`saved_title` (the
  page a pinned tab goes back to).
- `CompassProfile` (3): one per profile of the chain, in the home
  profile's account (see "Profiles").
- `CompassFavicon` (4): one per host (see "Favicons").

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
so. Profiles **created by sync** don't wait: the creator writes the new
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
  that carry this profile. **Recovery code**: the same seed in every
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
  (limits), `GET /healthz` and `/health-check`.
- It stores specifics opaquely (it can't read them anyway) and only
  interprets envelope fields, so types newer than its protos sync.
- Ships a `docker-compose.yml` with the server and a Caddy front for HTTPS,
  and `server/README.md` (VPS, home server behind Tailscale, backups,
  proxies). `compass sync-server` runs it locally for development.
- Anyone can run their own; the app's server field has no default. There
  is also a public instance at `https://sync.example.org/v2`.
  Everything on it is encrypted on the devices.

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
and last activity; a schema 1 database migrates on start. An older server refuses
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

## Decisions

1. **Server**: self-hostable; a public instance at
   `https://sync.example.org/v2` exists too, but the app has no
   default server.
2. **What syncs**: everything that can, Today and the archive included.
3. **First sync on a device with its own spaces**: merge.
4. **No accounts**: a sync chain. Devices join with a short pairing code
   from a device already syncing; a 12-word recovery code is the fallback.
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
