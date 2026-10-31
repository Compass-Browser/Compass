# Sync between devices

Status: slices 0, 1, 2, 2b and 3 done and merged into `sync-main`
(MacBook runs: "Slice 2 run on the MacBook", "Slice 2b notes", "Slice 3
notes"); 3b (Today and the archive) on `sync/slice-3b-today`; 5 (profiles)
on `sync/slice-5-profiles`, MacBook run pending ("Slice 5 notes"). Based on
Chromium 156.0.8078.48.

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
list) then sync with no new code. Compass's sidebar becomes one more data
type, `COMPASS_SPACES`. There are no accounts: as in Brave's "sync chain",
the first device makes a random seed; other devices get it by typing a
short pairing code shown on a device already syncing (or a 12-word recovery
code). Every device with the seed derives the same server identity (a
signing key) and the same encryption passphrase (Chromium's custom passphrase, so everything is
encrypted on the device). The server stores opaque blobs.

## Options considered

| Option | Verdict |
|---|---|
| **Chromium sync engine + own server (chosen)** | All Chromium types for free, proven conflict handling and E2E encryption (Nigori custom passphrase), sync-internals for debugging. Costs: a server to run, ~10 small patches, an auth override |
| CloudKit | No server, Apple ID auth. But macOS only (§1 wants Windows and Linux), needs the paid Apple Developer team signing and entitlements (we sign with a self-made identity), and covers only what we write ourselves: passwords/history would need a second mechanism |
| Our own JSON sync of `Compass Sidebar.json` (server or iCloud Drive folder) | Small to write, but only the sidebar, and file-level last-writer-wins loses edits made on two devices; iCloud Drive file conflicts are worse |
| Chromium's local sync backend file in iCloud Drive | `LoopbackServer` writes one file and assumes one writer; two Macs would overwrite each other |
| Map the sidebar onto `BOOKMARKS` | No new type, but splits, favorites, space colors and profiles don't fit, and bookmarks UI would show the tree |

## What syncs

Compass data (`COMPASS_SPACES`, new):

- Spaces: name, color, icon, order, auto-archive delay, profile.
- Pinned trees: folders (nested), tabs (URL, title, custom title), splits.
- Favorites.
- Today (unlike Arc; user, 2026-10-31): entries, folders and splits, with
  their last-active times. See "Today across devices".
- The archive (entries, archived-at times), capped to the last 30 days.
- Folder collapse state.
- Not synced, because it can't be or is per window: live state (which node
  has a tab, which window shows what), sidebar width and collapse, the
  selected entry.

Chromium types, on by default: `PASSWORDS`, `HISTORY` (with its delete
directives; slice 2b, see "Slice 2b notes"), `AUTOFILL` (and
addresses), `EXTENSIONS`, `EXTENSION_SETTINGS`, `PREFERENCES`
(only syncable prefs), `SESSIONS` (open tabs per device), `DEVICE_INFO`
(device list), `SEARCH_ENGINES`, `READING_LIST`, `BOOKMARKS`, `NIGORI`.

Off (they need Google back ends or don't apply): the `AUTOFILL_WALLET_*`
types, `PLUS_ADDRESS*`, `SHARING_MESSAGE`, `SEND_TAB_TO_SELF` (needs FCM),
`SECURITY_EVENTS`, `USER_CONSENTS`, `WEBAUTHN_CREDENTIAL` (needs Google's
security domain secrets), `SAVED_TAB_GROUP` / `SHARED_TAB_GROUP_*` /
collaboration types (our folders replace groups), `POWER_BOOKMARK`,
`PRODUCT_COMPARISON`, `THEMES` (space colors theme the window), `APPS`,
incoming password sharing. Exact list from the spike (slice 0).

## Identity: a sync chain, no accounts

- **Seed**: 16 random bytes (128 bits, the strength of the AES keys that
  encrypt the data), made by the first device.
- **Recovery code**: the seed as 12 BIP39 words, shown once when sync
  starts so the user can save it (a password manager, paper), and again
  from settings behind Touch ID. Only needed when no synced device is left:
  lose every device and the code, and the synced data is gone. Say so in
  the UI.
- **Adding a device** uses a pairing code, not the words (see "Pairing").
- From the seed, with HKDF-SHA256 (BoringSSL):
  - `info="compass-sync-auth"` → an Ed25519 key pair. The public key is
    the chain's ID on the server.
  - `info="compass-sync-passphrase"` → Chromium's custom passphrase
    (base64). Set with `SyncUserSettings::SetEncryptionPassphrase()` on the
    first device and `SetDecryptionPassphrase()` on the others. All types
    encrypted; the server sees types, IDs, versions, sizes, times.
- **Auth to the server**: go-sync's own token format, kept as is
  (proven in slice 0, no server change): per request,
  `base64url_padded(HEX(ts_ms) | HEX(ed25519_sign(ts_ms)) | HEX(pubkey))`,
  where `ts_ms` is the ASCII decimal Unix time in ms, sent as
  `Authorization: Bearer <token>`. The server checks the signature and a
  ±24 h window (go-sync's `auth/auth.go`); the chain ID is the hex
  public key. No passwords, no email.
- **Derivations** (fixed, both slices use them): seed = 16 bytes;
  `HKDF-SHA256(ikm=seed, salt="", info="compass-sync-auth", L=32)` = the
  Ed25519 private seed; `HKDF(..., info="compass-sync-passphrase", L=32)`,
  base64 (standard, padded) = the custom passphrase. Profile accounts
  (slice 5) append `":" + profile_uuid` to both infos: each has its own
  account and its own passphrase ("Slice 5 notes").
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
  same Keychain key as passwords), in a profile pref.
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
   `7-482913`, and a QR of the server URL and code (for phones later).
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

#### Pairing API (contract between slices 1 and 2)

Under go-sync's prefix, next to `/v2/command/`. JSON bodies, UTF-8.
Binary fields are standard base64.

- `POST /v2/compass/pair` with `Authorization` (A's sync token) →
  `201 {"slot": 7, "expires_in": 300}`. The lowest free slot ≥ 1. Opening
  a new slot closes the chain's previous one (one open slot per chain).
- `POST /v2/compass/pair/{slot}/join` (no auth; B) →
  `200 {"b_token": "<random 128-bit, hex>"}`. Only the first join
  succeeds; after it, `409`. Unknown or expired slot: `404`. Rate limit:
  10 joins per IP per 10 min, then `429`.
- `POST /v2/compass/pair/{slot}/msg` with body
  `{"from": "a"|"b", "data": "<base64, ≤ 4 KiB>"}`. From A: the
  `Authorization` of the chain that opened it. From B: header
  `X-Compass-Pair-Token: <b_token>`. → `200 {"seq": n}`. At most 8
  messages per side, then `429`.
- `GET /v2/compass/pair/{slot}/msg?for=a|b&after=<seq>` (same auth as
  above, for the reader) → long poll up to 25 s: `200 {"messages":
  [{"seq": n, "data": "..."}]}` (possibly empty on timeout). Sequence
  numbers count per sender, from 1.
- `DELETE /v2/compass/pair/{slot}` (A's auth, or B's token) → `204`. It
  closes the slot. Closed or expired slots answer `404` everywhere.
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

Framing as slice 2 implements it (`components/sync/pairing.h`); one relay
message per step:

- 1 and 2: the raw 32-byte SPAKE2 message (BoringSSL's SPAKE2 over
  edwards25519; its key is 64 bytes, all of it the HKDF input, salt empty,
  32 bytes per subkey).
- 3: `MAC_b (32) || nonce (12) || AES-256-GCM(K_enc, name, ad="compass-pair-b")`.
- 4: `MAC_a (32) || verdict (1 byte: 0x01 allow, 0x00 deny)`, then on allow
  `nonce (12) || AES-256-GCM(K_enc, {"seed": base64(16 bytes), "profiles": [...]}, ad="compass-pair-a")`.
  Since slice 5, `"profiles"` is the chain's list, `[{"uuid": "<lowercase
  UUID>", "name": "Work", "avatar": 26, "home": true}, ...]` (`avatar` only
  if set, `home` only on the home profile); a joiner ignores entries it
  can't read, and treats an empty list as unknown.
- `host` in the SPAKE2 names is `GURL::host()` of the server URL each side
  has (no port, lowercase).
- A doesn't close the slot after sending 4 (B must still read it); B closes
  it once it has 4. A closes it on a wrong MAC, cancel or expiry.
- Vectors for K = `00 01 … 3f`: K_enc `5144144d…7a857653`, K_confirm_a
  `d71f8461…63ef6a2a`, K_confirm_b `46d4fef6…dd61a05a`, MAC_a
  `5640ef87…7f3366f8`, MAC_b `d0ccb754…12ff0ac4` (full values in
  `pairing_unittest.cc`, `PairingKeysTest`).

"Use a recovery code" on B is the fallback: type the 12 words (BIP39
autocomplete, checksum checked as you type).

### Plugging it into Chromium

The engine wants a signed-in account (`SyncAuthManager` gets an OAuth token
from `IdentityManager`). Two ways. **Slice 0 chose route 2** (see "Slice 0
findings"); route 1 stays as the fallback.

1. **Synthesized account**: `SyncAuthManager` (patch or
   `chromium_src` override) returns a `SyncAccountInfo` with
   `gaia = "compass:" + hex(pubkey[0..16])` and our signed token from
   `RequestAccessToken()`, and Compass sets that account as primary in
   `IdentityManager` without Gaia (accounts mutator, no cookies). Brave
   does this. Keeps account-keyed prefs (passphrase bootstrap token,
   selected types per account) working.
2. **Local-sync path (chosen)**: `kEnableLocalSyncBackend` already runs the
   engine with no identity at all, but `EngineComponentsFactoryImpl` then
   builds a `LoopbackConnectionManager`; a hook in
   `SyncServiceImpl::TryStartImpl()` swaps in our own engine components
   factory, whose connection manager posts to the server and signs each
   request. Local sync limits types (`LocalSyncSupportedTypes()`; History is
   the one we want back). The feared passphrase prompt on every launch
   doesn't happen (slice 0).

Either way: the server URL comes from pref `compass.sync.server_url`
(hook in `GetSyncServiceURL()`; `--sync-url` keeps working), and Google
sign-in UI and sync promos stay hidden.

### Slice 0 findings

Run 2026-10-31 on the MacBook (out/Debug, DCHECKs on), branch
`spike/sync-0` at `b8e729a`, against brave/go-sync `31ad41a` on the Mac
mini over the LAN (`http://192.168.1.20:8295/v2`, plain HTTP accepted).
Steps: `docs/design/sync-spike-0-steps.md` on the branch; evidence in
`~/compass-sync-spike-evidence.tgz` on the MacBook.

- **Route 2 works with two small upstream patches**:
  `components/sync/service/BUILD.gn` (adds `//compass/components/sync_spike`
  sources to the `service` target, plus a BoringSSL dep; a separate target
  would be a dependency cycle) and `sync_service_impl.cc` (include; factory
  swap in `TryStartImpl()`; an `Initialize()` hook). It compiled on the
  first try (about 425 actions incremental). Route 1 wasn't built: it needs
  `AccountsMutator`/`PrimaryAccountMutator`, a quiet account reconciler and
  other Gaia services, and a `RequestAccessToken()` patch.
- **No server change**: the client signs go-sync's own token format
  (Ed25519 over a timestamp). All requests got 200 except the very first
  (52 bytes, `CONNECTION_UNAVAILABLE`, 1.6 s before the engine was ready),
  then fine. go-sync logged no errors.
- **Mode**: sync-the-feature stays off (`sync_consent=0`,
  `IsSyncFeatureEnabled()` false, first-time setup not complete), yet the
  data types run as full sync. Synced passwords land in the profile store
  (`storedIn: DEVICE`), not an account store, which is what we want.
- **Both directions**: a bookmark and a password from A reached B; a
  bookmark from B reached A. About 14–20 s, with a manual refresh trigger
  (so change notifications, slice 6, matter).
- **Encryption**: custom passphrase set on A, accepted on B, and a wrong
  one on C gives a clean "Needs passphrase" state (cryptographer pending
  keys, data types stopped, nothing decrypted). **The passphrase survives a
  restart**: A came back with `kCustomPassphrase` and nothing required,
  without the passphrase being supplied again. Sync's own Nigori storage
  keeps the keys, so the missing Gaia-keyed bootstrap token doesn't matter.
  The server sees 37 encrypted types.
- **Running types (19)**: Bookmarks, Preferences, Priority Preferences,
  Passwords, Autofill, Autofill Profiles, Autofill Wallet Credential,
  Themes, Extensions, Extension settings, Apps, App settings, Web Apps,
  Search Engines, Sessions, Device Info, Reading List, Saved Tab Group,
  WebAuthn Credentials. Slice 2 turns off the ones this doc doesn't want
  (Wallet Credential, Themes, Saved Tab Group, WebAuthn, Apps).
- **Not running in local mode**: History, History Delete Directives, Send
  Tab To Self, Contact Info, Sharing, password sharing, User Events/Consents,
  Security Events, the Wallet types, shared tab groups and collaboration.
  Of these we want **History** (and its delete directives): slice 2 widens
  `LocalSyncSupportedTypes()` (a third small patch) and checks why upstream
  left it out.
- **No Google traffic from sync**: every Google host seen with sync on
  (`update.googleapis.com`, `edgedl.me.gvt1.com`, `www.google.com`,
  `clients2`, `accounts`, `safebrowsing`) also shows with `--disable-sync`;
  `clients4.google.com/chrome-sync` and `oauth2.googleapis.com` never
  appear. That baseline Google traffic (component updater, Safe Browsing,
  DNS-over-HTTPS) is a separate privacy task, not sync's.
- **go-sync and Chromium 156**: no protocol errors with 19 types, including
  ones newer than go-sync's protos (WebAuthn, Saved Tab Group), at least
  with small data.
- **Fix in the steps doc**: `$CDP` needs bash (zsh doesn't word-split), and
  `netlog_hosts.py` must accept net logs cut short by SIGTERM (a missing
  final `}`).

### Slice 2 notes

Branch `sync/slice-2-chain` (on `sync-main`). Written without a compiler;
the MacBook build and run are in
[sync-slice-2-steps.md](sync-slice-2-steps.md).

**Code.**

- `//compass/components/sync:chain`: seed, BIP39 recovery code,
  derivations, tokens (`chain.h`, `bip39.h`), pairing on both sides with a
  transport interface (`pairing.h`) and its HTTP implementation
  (`pairing_network_transport.h`).
- `//compass/components/sync:engine`: the spike's engine components,
  renamed (`CompassEngineComponentsFactory`, a connection manager that signs
  each request), `SyncCredentials` (the chain key shared between the
  browser side and the sync sequence; the first request waits up to 20 s
  for the seed to decrypt) and `sync_hooks.h`, what `SyncServiceImpl`
  calls. `//components/sync/service` depends on this target: unlike the
  spike's observer, nothing in it needs `SyncService`, so it's a separate
  target with no cycle, and the patch is one `deps` line instead of a
  source list.
- `//compass/browser/sync`: `CompassSyncService` (keyed service, created
  with each regular profile): the seed (`compass.sync.seed`, os_crypt_async,
  base64) and server (`compass.sync.server_url`) prefs, passphrase, type
  selection, refresh on focus (`ProfileBrowserCollection`) and wake
  (`PowerSuspendObserver`, at most every 10 s), pairing. `SyncController`
  is its interface for the page.
- `chrome://compass-sync` (`browser/ui/webui/sync/`,
  `resources/sidebar/compass_sync.{html,ts}`).

**When sync runs.** The engine talks to the chain's server when the
profile has `kEnableLocalSyncBackend` (route 2), a seed and a server URL
(`--sync-url` wins over the pref). `SyncServiceImpl` reads
`kEnableLocalSyncBackend` once, when the profile loads, so "Start sync",
joining and "Leave sync" take effect after a restart; the page says so and
has a "Restart Compass" button. Turning sync on also sets
`kLocalSyncBackendDir` (to `<profile>/Compass Sync`, never written): on
macOS `SyncServiceFactory` doesn't create a local-sync service without it.
Leaving clears the seed and the local-sync pref; the next launch's sync
service, not signed in, clears sync's metadata as for a signed-out user and
keeps the data.

**Passphrase.** At each start with a chain, `CompassSyncService` selects
no data type (Nigori and Device Info still run), so nothing is committed
before the chain's passphrase is in place (the
spike set it only once the types were running, so a first device's
initial commits went out under the server-known keystore key). With the
engine up: if the server's Nigori needs a passphrase (a joining device, or
keys lost), `SetDecryptionPassphrase()`, once per time it's asked; if the
passphrase type isn't explicit yet (first device), `SetEncryptionPassphrase()`;
once it's explicit, the wanted types are selected and start. (A
`SyncSetupInProgressHandle` doesn't work for this: it holds Nigori back
too, so the passphrase type never becomes known; seen on the MacBook.) Any device sets
it, not only the one that made the chain: a device that finds no custom
passphrase on the server (a wiped server) must not upload in the clear,
and two devices racing to set it converge, since the passphrase is the
same and the loser is asked to decrypt. A rejected passphrase (another
chain's data on the server) keeps the handle: nothing syncs, and the page
says why.

**Types.** No feature flag turns off the unwanted types in 156:
Themes, Apps (with App settings and Web Apps) and Saved Tab Group
controllers are built unconditionally, and `WEBAUTHN_CREDENTIAL` belongs to
the Passwords selectable type. So `CompassSyncService` deselects the
selectable types Apps, Payments (Wallet types, including Wallet
Credential), Product Comparison, Saved Tab Groups and Themes, and a
one-line patch passes `WEBAUTHN_CREDENTIAL` as a disabled type to
`CommonControllerBuilder::Build()` in `sync_service_factory.cc`.
`RegisterFeatureOverrides()` is unchanged. Expected running types (15):
Bookmarks, Preferences, Priority Preferences, Passwords, Autofill,
Autofill Profiles, Extensions, Extension settings, Search Engines,
Sessions, Device Info, Reading List, plus Nigori (control). To check on
the MacBook run.

**History: not in this slice, by upstream design.** Widening
`LocalSyncSupportedTypes()` wouldn't be enough, so it isn't patched:

1. `HISTORY` and `HISTORY_DELETE_DIRECTIVES` are
   `EncryptionPolicy::kNeverEncrypted` in `data_type.cc` ("History Sync is
   disabled if encryption is enabled"): Google's server reads history for
   Web & App Activity, so upstream never encrypts it.
2. Their controllers (`history_data_type_controller.cc`,
   `history_delete_directives_data_type_controller.cc`) return
   `kMustStopAndClearData` whenever `IsEncryptEverythingEnabled()`, which a
   custom passphrase always is. Compass's chain always has one.
3. `HistoryDataTypeController` also uses
   `AccountManagedStatusPolicy::kDisallowEnterprise`, and with local sync
   the account's managed status stays `kPending` (no account), which means
   `kMustStopAndKeepData`.
4. The local-sync exclusion itself (`LocalSyncSupportPolicy::kUnsupported`)
   carries no comment; local sync was built for Windows roaming profiles,
   and 1-3 already make history unusable with any custom passphrase, so
   it's likely just consistent with them (an inference, not documented
   upstream).

Syncing history end-to-end encrypted would take: `kNeverEncrypted` →
encryptable for both types in `data_type.cc`, `kSupported` for local sync
there too, and the two controllers' passphrase check and the managed-status
policy relaxed: four upstream files, two of them outside sync, and a
behaviour upstream deliberately avoids (history sync's server-side
features don't apply to us, so encryption itself should be fine). Proposed
as its own slice ("2b: History"), after checking how Brave's tree handles
the same three blockers. Done in slice 2b ("Slice 2b notes").

**FakeServer integration test: skipped.** `FakeServer` itself works with
route 2 (`SyncServiceFactory`'s HTTP post provider override reaches
`CompassEngineComponentsFactory` the same way), but the two-client harness
(`SyncTest`, `SyncServiceImplHarness`) signs each client in with a test
Gaia account and waits on states that assume one; local sync has none, and
upstream only tests it single-client (`LocalSyncTest`). A two-client
local-sync harness is a slice of its own; slice 3's
`TwoClientCompassSpacesSyncTest` needs it too, so it belongs there.
Pairing and chain logic are covered by unit tests; the two-client check is
the MacBook run.

**Contract (slice 1 ↔ 2) as built, and proposed changes.** The client
follows "Pairing API" as written and slice 1's details (403 for another
chain or a wrong `b_token`; 404 before auth and for a long poll whose slot
closes; 413/400 for bad data; 429/503 on open = try later). Open points,
none blocking:

1. B can't tell why a slot closed (A saw a wrong code, A canceled, or it
   expired): all are 404. Proposed: `DELETE` takes `?reason=mismatch|cancel`
   and later requests answer `410 {"reason": ...}`, so B can say "wrong
   code" instead of "the other device ended pairing".
2. `host` in the SPAKE2 names: two devices that reach the server by
   different names (IP vs hostname, Tailscale name) compute different keys
   and fail as "wrong code". Proposed: have `POST /pair` return a
   canonical server name (`"server": "sync.example.com"`) and `join` echo
   it, and bind that instead of what each side typed.
3. `join` doesn't return the slot's remaining lifetime; B uses its own
   5.5-minute timeout. Proposed: `{"b_token", "expires_in"}`.
4. The framing of messages 3 and 4 ("then") and the denial encoding were
   open; fixed here as above.

### Slice 2 run on the MacBook

2026-10-31, out/Debug, branch `sync/slice-2-chain` at `0499ea3`, against
the slice 1 server on the Mac mini. Evidence in
`~/compass-sync-slice2-evidence.tgz` on the MacBook.

- Fixes the run needed: a hand-written patch with a missing context line
  (all patches regenerated with `git diff --full-index`), two test compile
  errors (`base/containers/contains.h` is gone in 156; a missing
  `pref_registry_simple.h`), a missing test helper, and the setup-handle
  deadlock (see "Passphrase" above).
- 42 unit tests pass. No crash or DCHECK in any instance.
- A started a chain, set the passphrase before any data type ran, then
  synced. B joined by recovery code and got A's bookmark and password
  (about 20 s with a refresh); B's reached A (about 27 s). Passwords land
  in the profile store.
- Pairing: right code + Allow → C joins and has the data after a restart;
  wrong code → both sides fail, D stays off; Deny → D stays off. With a
  wrong code, B says "The other device ended pairing" instead of "wrong
  code": the contract change "a closed slot says why" fixes it (later).
- Running types: Bookmarks, Preferences, Priority Preferences, Passwords,
  Autofill, Autofill Profiles, Extensions, Extension settings, Search
  Engines, Sessions, Device Info, Reading List. The unwanted ones and
  History don't run. Custom passphrase on every instance.
- No Google host from sync (same set as with sync off); the server logged
  no errors.

### Slice 2b notes

**MacBook run (2026-10-31, out/Debug, `1ee7fd7`): passed.** 22 unit tests
pass (the override is in the build). History and History Delete
Directives run encrypted (custom passphrase). A's visits reached B with
the same visit times within about a minute; a page removed on B left A
within about 35 s, applied by the directive handler. No crash; no Google
host beyond the baseline. Two side findings: sync-internals shows History
with 0 entries although the visits sync (history entities aren't kept as
live entries), and `chrome://history`'s `queryHistory` returns nothing in
headless runs even without sync (a test-tooling problem, also in a
no-sync control); check the history page by eye.

Branch `sync/slice-2b-history` (on `sync-main`). Written without a
compiler; the MacBook build and run are in
[sync-slice-2b-steps.md](sync-slice-2b-steps.md).

**What turned History off, and what changes.** The four blockers of
"Slice 2 notes" plus a fifth found here:

| Blocker | Change | Gate |
|---|---|---|
| 1. `kNeverEncrypted` for both types | `chromium_src/components/sync/base/data_type.cc` wraps `EncryptableUserTypes()` to add HISTORY and HISTORY_DELETE_DIRECTIVES | none needed: only read with a custom passphrase, under which upstream stops history unless local sync is on (row 2) |
| 4. `LocalSyncSupportPolicy::kUnsupported` | same override wraps `LocalSyncSupportedTypes()` | only read with local sync on |
| 2. Controllers stop on `IsEncryptEverythingEnabled()` | patches to `history_data_type_controller.cc` and `history_delete_directives_data_type_controller.cc`: not when `SyncService::IsLocalSyncEnabled()` | local sync |
| 3. Managed status `kPending` (no account) stops HISTORY | patch to `history_data_type_controller_helper.cc`: `kDisallowEnterprise` passes when `IsLocalSyncEnabled()` | local sync |
| 5. Delete directives are only made when a `WebHistoryService` exists (`WebHistoryServiceFactory` returns none with local sync), so a deletion never left the device | patch to `browsing_history_service.cc` (chrome://history "Remove"): also with `IsLocalSyncEnabled()`; patch to `history_service.cc` (Clear browsing data, extensions API): without web history, ask the directive handler anyway | the first: local sync. The second has no `SyncService`; the handler only commits while HISTORY_DELETE_DIRECTIVES runs (else it returns an ignored error), and outside local sync that type runs only together with web history, so in practice local sync too |

The `kPreconditionsMet` paths still honour `SavingBrowserHistoryDisabled`
(policy). `data_type.cc`'s table is untouched, so the override doesn't
conflict with slice 3's patch to that file (a new type); it's the tree's
first `chromium_src` override (`build/redirect_cc.py`, wired by `compass
gen`). Upstream's `DataTypeTest.LocalSyncSupportedTypes` and
`LocalSyncTest.ShouldStart` now fail by design (they assert History is
out of local sync).

**Type selection.** Nothing to change in `CompassSyncService`: `kHistory`
was never in `kUnwantedTypes`, so `SelectTypes()` already selected it and
local sync filtered it out; `HoldTypes()` holds it until the passphrase like
the rest. kHistory's other types (USER_EVENTS, JOURNEY, and DICTIONARY
when its feature is on) stay out: local sync doesn't support them.

**Brave.** Brave syncs History through an account (route 1), with
subclassed controllers (`BraveHistoryDataTypeController`,
`BraveHistoryDeleteDirectivesDataTypeController`) whose
`GetPreconditionState()` always returns `kPreconditionsMet`, swapped in by a
`chromium_src` override of `common_controller_builder.cc` (`#define` of the
class names). Its `data_type.cc` override adds HISTORY (and DEVICE_INFO)
to `EncryptableUserTypes()`, but not HISTORY_DELETE_DIRECTIVES, and it
disables URL delete directives (`CreateUrlDeleteDirective()` returns
false), presumably because those would carry a URL in the clear. Its
`LowPriorityUserTypes()` override makes delete directives low priority and
History regular (go-sync issue #178): on a first sync, directives applied
before the history they delete arrives leave the entries in place. Brave
has a `WebHistoryService` (an account), so its directives come from
upstream's code paths; ours need blocker 5. Compass encrypts both types,
so URL directives stay on, and keeps upstream's priorities (that override
isn't gated, it would change every profile; see "Deletions" for the
consequence).

**Server.** No change needed. go-sync's history rules work on encrypted
entities (the type comes from the empty specifics field, the ID is the
client tag hash, updates go by tag); `server/server/e2e_history_test.go`
covers it (commit, update by tag, directive tombstone, a later joiner).
Limits as shipped: HISTORY and directives expire 14 days after their last
write (`HistoryExpirationIntervalSecs`, purged hourly); at most 30,000
history items per chain in that window (counted in four 3.5-day periods),
past which the server drops new history silently (answers SUCCESS) so
other types keep syncing; history counts toward the 100k entities and 200
MB of the chain. Encrypted visits are about 1-2 KB, so 30k is about
30-60 MB. That's roughly 2,000 visits a day for the whole chain, enough
for a few devices; a heavy user could hit it, and `COMPASS_SYNC_MAX_ENTITIES`
can only lower it. Possible later: make retention and the history quota
configurable (the period counts derive from the retention constant).

**Consequences to know.**

- Only visits made while History syncs are uploaded (upstream: local
  history is never uploaded), and a device joining sees the other devices'
  last 14 days at most. Each device keeps what it received for its own
  local retention (90 days).
- Deletions: the server can't delete the HISTORY entities a directive
  covers (it can't read either), and the bridge never commits deletions.
  Devices that are syncing apply a directive when it arrives. But each
  device drops processed directives from the server at its next start
  (act-once), so a device that joins later, while the deleted visits are
  still within the server's 14 days, gets them back. Fixes for later: a
  delete commit for the HISTORY entities from the device that deletes
  (patch `HistorySyncBridge::OnVisitDeleted` under local sync), and/or
  Brave's priority swap.
- A device offline for more than 14 days misses directives too.

**Tests.** `components/sync/history_sync_unittest.cc` (target
`//compass/components/sync:history_unit_tests`, in `compass_unittests`):
the override, both controllers' preconditions with and without local sync,
directives from `HistoryService` and `BrowsingHistoryService` without web
history. Filter: `CompassHistorySyncTypesTest.*:CompassHistoryControllerTest.*:CompassHistoryDeleteDirectivesTest.*`.
Go: `go test ./server -run TestEndToEndEncryptedHistory`. The two-client
check is the MacBook run (history from A on B, a removal on B reaches A).

### Slice 3 notes

**MacBook run (2026-10-31, out/Debug): passed.** 75 unit tests pass at
`61e11f0` (the run found and fixed: UTF-16 in gtest messages; an entry
rescued from a folder deleted elsewhere was re-placed by its old position
and went first instead of last). Three headless instances: a sidebar built
on A (space, color, renamed and collapsed folder, moved tab, favorite)
reached B identically with no echo; B's move and delete reached A (its
live tab closed) in about 10 s; concurrent moves of X into Y and Y into X
converged with nothing lost; C with its own space and favorites joined and
all three converged (Home, Travel, Mine; the shared favorite once).
Compass Spaces runs encrypted; the initial merge always came after the
passphrase. No crash.

Branch `sync/slice-3-spaces` (on `sync-main` `019b52c`). Written without a
compiler; the MacBook build and run are in
[sync-slice-3-steps.md](sync-slice-3-steps.md).

**The type.** `syncer::COMPASS_SPACES`, the last user type.
`EntitySpecifics.compass_spaces` is **field 970000** (upstream's recent
fields are 1.4M-1.7M, its deprecated ones lower; 970000 is unused and far
from both). `DataTypeForHistograms::kCompassSpaces = 200`, kept as
`kMaxValue` (on a rebase, upstream's new value goes below it and
`kMaxValue` stays ours). Encryption policy "encrypted if custom
passphrase" (always, with the chain), local sync supported (route 2 needs
it), regular priority, medium nudge delay. The patches are those listed in
"Patches for a new data type" except `user_selectable_type` (only its
static_assert) and `chrome_sync_controller_builder.cc` (not needed, see
below), plus `data_type_tracker.cc` (two exhaustive switches) and
`chrome/browser/sync/BUILD.gn`. All generated from pristine 156 files and
checked with `git apply --check`; the list is in the steps doc, step 0.
`proto_value_conversions_unittest.cc`, `sync_service_factory_unittest.cc`
and `sync_test.cc` have the same static_assert (64 types) but aren't built
by Compass; they'd need the same one-line change if those suites are ever
built.

**The proto** lives in `//compass/components/sync/protocol/`, built by
`//components/sync/protocol` (its `protocol_sources.gni` lists it by its
`//compass` path; `proto_in_dir` is `//`, so the import path is
`compass/components/sync/protocol/...`). Deviations from the sketch above:
proto2 with `optional` fields, as all sync protos; the enums are `int32`
fields with 0 = unset (`section` 1 PINNED, 2 TODAY, 3 FAVORITES, 4 ARCHIVE;
`type` 1 TAB, 2 FOLDER, 3 SPLIT; `color` SpaceColor + 1), because proto
enums need `ProtoEnumToString()` overloads (one more upstream patch) and an
unknown proto2 enum value would read as the default (PINNED); and
`position`/`directory_position` are `sync_pb.UniquePosition` messages, not
bytes. `icon`, `profile_uuid`, `last_active_time` and `archived_time` exist
but aren't set; values another client sets are kept.

**Not a selectable type; always on with the chain.** A "Spaces" toggle
would mean `UserSelectableType::kCompassSpaces` and its cases in
`user_selectable_type.cc`, `sync_prefs.cc`, `sync_policy_handler.cc`,
`browsing_data_policies_utils.cc` and more (eight upstream files switch on
the enum). Instead `COMPASS_SPACES` is in `AlwaysPreferredUserTypes()`,
and its controller (`SpacesDataTypeController`, `compass_spaces_sync.cc`)
returns `kMustStopAndKeepData` from `GetPreconditionState()` unless the
profile's `CompassSyncService::AreDataTypesReleased()`: the chain runs and
the passphrase is in place (the same moment slice 2 selects the other
types). `HoldTypes()`, `SelectTypes()` and `Leave()` call
`DataTypePreconditionChanged(COMPASS_SPACES)`. So it never commits before
the passphrase. A toggle can come with slice 4's settings page (a pref the
precondition reads, no upstream change).

**Controller registration.** `//chrome/browser/sync` can't depend on
`//compass/browser/sync` (which depends on it), so a tiny target,
`//compass/components/sync:controllers_hook`, holds a factory callback:
`CompassSyncServiceFactory`'s constructor (run with the other factories,
before any profile) registers it, and `CreateControllers()` in
`sync_service_factory.cc` appends what it returns (3 lines and an
include). The controller's delegates are `ProxyDataTypeControllerDelegate`s
whose provider creates the bridge on first use: nothing is built for a
profile that never syncs.

**Node IDs are now saved** (`"id"` per node in `Compass Sidebar.json`;
sidebar_serializer). Before, they were random per session, so they couldn't
be client tags. Older files load as before and their nodes get fresh IDs
once; a missing, invalid or (within its space) duplicate ID gets a fresh
one. `SpaceModel::AddTab()/AddFolder()` take an optional ID, and
`AddSplit()` makes an empty split with one (loading and sync add the
panes), so splits keep theirs too.

**The bridge** (`CompassSpacesSyncBridge`, `browser/sync/`,
`:spaces_bridge`, no window code) is owned by the profile's `SpaceService`
through `SpaceService::Attachment`, with a small holder
(`compass_spaces_sync.cc`) that supplies what needs windows: deleting a
space (`compass::DeleteSpace()`, which switches windows away first; plain
`SpaceList::RemoveSpace()` would trip `WindowFeatures`' CHECK) and moving
entries across spaces with their live tabs (`compass::MoveEntry()`). How it
works:

- A **mirror**: each entity's specifics as the server last had them, in the
  bridge's `DataTypeStore` (data records next to the metadata). The sidebar
  file stays the source of truth for the tree.
- **Local changes** aren't tracked one by one. Any change to the spaces
  posts one pass that computes every entity from the model and `Put()`s
  those that differ from the mirror, `Delete()`s mirror entities the model
  no longer has. Simple and robust (it also catches changes made while sync
  wasn't running); a few hundred nodes per pass is cheap.
- **Remote changes** go into the mirror first, then into the model through
  its normal methods. The next pass finds the model equal to the mirror, so
  nothing echoes; what it does commit are the model's fix-ups. The
  "sync-origin flag" is the bridge's `applying_remote_`, which keeps its own
  observers quiet during the apply; other observers (store, UI, tab strip)
  react as to any change.
- **Order**: a pass keeps the positions of the longest run of siblings
  already in increasing order and gives the others new ones between their
  neighbors (`internal::AssignPositions()`), so a move commits one entity.
  Suffixes come from the client tag hash, as bookmarks do. Positions the
  bridge keeps aren't re-encoded, so they compare equal.
- **Apply rules** (sync.md above): unknown parent → the node waits at the
  end of its section's root, not committed (it's "parked" as long as the
  mirror names a parent the model doesn't have), and moves in when the
  parent arrives; parent deleted → stays at the root and that's committed.
  Cycle or invalid placement → the section's root, committed. Deletes
  remove the subtree (one tombstone per node, the sender's pass sends them
  all); entries added here to a folder deleted elsewhere are kept at the
  root. A split the batch leaves with fewer than two panes is dissolved. A
  remote URL change of a live tab updates the entry only (no navigation);
  a remote delete of a live entry closes its tab (`TabStripBridge` does, as
  for a local delete).
- **Pin/unpin**: unpinning (a move to Today) is a delete for other devices
  in this slice. A remote entity in section TODAY (from a 3b client) moves
  the local entry to its space's Today; ARCHIVE removes it. Neither is
  deleted from the server by this version.
- **Entities this version can't place** (a node of a space it doesn't have,
  a non-tab favorite, Today/archive) stay in the mirror and are never
  deleted from the server; a node waits for its space and appears when the
  space does.
- **Spaces**: name, color, archive delay, order. The order is the profile's
  `SpaceList` order; the holder keeps it equal to the switcher's
  (`SpaceDirectory`) order for the profile's spaces, both ways. A remote
  delete of the last space leaves it (and the next pass puts it back).
- **Initial merge** (`MergeFullSyncData`): union. The chain's spaces come
  first, this device's after; a local favorite whose URL the chain already
  has gives way to the chain's; a joining device's untouched starting space
  ("Home", nothing pinned) gives way to the chain's first space, its Today
  entries moving there. Same-ID entities (data synced before) are the
  chain's version.
- Leaving sync (`ApplyDisableSyncChanges`) clears the mirror and metadata,
  keeps the spaces.

**Profiles**: only the profile running the chain syncs (each profile's
sync service gets the controller; only one has a chain in slices 1-4).
`profile_uuid` isn't set.

**Arc import (proposed, not done).** The importer adds nodes with fresh
random IDs (`arc_import/sidebar_import.cc`), so importing the same Arc
data on two Macs doubles every entry after a merge. Arc's items have IDs
(`Item::id`, `Space::id`): pass `UUIDv5(compass-arc namespace, Arc ID)` to
the new `AddTab()/AddFolder()/AddSplit()` ID parameter and to
`SpaceList::AddSpace()`. Needs a v5 helper (SHA-1, `crypto/`), skipping
items whose derived ID already exists (a second import on the same Mac),
and the reused starting space keeps its own ID. Small, but not trivial
enough for this slice.

**Two-client integration test: not written.** As slice 2 found, `SyncTest`
signs every client into a Gaia test account and its harness waits on
states that assume one; local sync has no account, so a two-client
local-sync harness would be its own project. The bridge's unit tests run
two and three bridges against an in-memory server instead (create, rename,
move, collapse, favorites, cycle, parking, invalid placement, subtree and
concurrent deletes, unpin, the merge, restart), and the MacBook run is the
end-to-end check.

**Known limits.** A live pinned tab commits its URL and title on each
change, and a tab live on two Macs at different pages takes the entity back
on its next update (no throttling until 3b). Partial delivery of a large
folder delete (over more than one commit message) can rescue some children
to the root before their tombstones arrive.

**3b slots in** by syncing the Today root (section TODAY, with
`last_active_time`) in the same pass and the archive as ARCHIVE entities;
the TODAY/ARCHIVE handling above then becomes normal placement.

### Slice 5 notes

Branch `sync/slice-5-profiles` (on `sync-main` `63d3418`). Written without
a build; the MacBook build and run are in
[sync-slice-5-steps.md](sync-slice-5-steps.md). Every changed C++ file was
syntax-checked here with the checkout's clang, `out/Debug`'s flags (Chromium
style and raw_ptr plugins on) and patched copies of the generated headers
(the sync protos, `data_type.h`, the page's mojom); `compass_sync.ts` was
type-checked with the checkout's tsc. Nothing was linked or run.

**Accounts.** Every profile the switcher lists (SpaceDirectory) is in the
chain. The profile that starts or joins it is the **home profile**: its
account is the chain's own (`compass-sync-auth`, as in slices 2-4, so their
chains carry on unchanged). Every other profile has its own:

- `compass.sync.profile_uuid`: a random UUID, made once when the profile's
  `CompassSyncService` is first built (Compass had none: Chromium profiles
  have no stable ID beyond their directory). Mapping at join replaces it.
- `compass.sync.profile_account`: true for every profile but the home.
- Auth key `HKDF(seed, info="compass-sync-auth:" + uuid)`, passphrase
  `HKDF(seed, info="compass-sync-passphrase:" + uuid)`, base64 as for the
  chain's. Each profile stores the seed itself (the home's encrypted value,
  copied: the os_crypt key is the same for every profile), so
  `SyncCredentials`, `sync_hooks` and the engine are unchanged.

**Passphrase per profile (decided).** Each account derives its own rather
than sharing the chain's. It costs nothing (one more derivation; each
profile has its own Nigori anyway) and buys domain separation: an entity
the server copies from one account into another doesn't decrypt there, so a
malicious server can't move Work's passwords into Personal or replay one
account's data into another. It doesn't protect a profile from someone with
the seed, and isn't meant to: the seed opens every account. Vectors (seed
`00 01 … 0f`, profile `2b5c3e1a-0000-4000-8000-000000000001`; computed with
Go's `crypto/hkdf` and `crypto/ed25519`; `ChainTest.ProfileAccountVectors`
and `ChainTest.DeriveAuthKey`):

| | |
|---|---|
| auth key | `1b28e57a6e3ca95bc1b4b9c30f10f8d554ef09eed00844b52048aea6d90f4cf4` |
| public key = account ID | `f1fe31e2c21e376e8d3b2ef57560b8466b37486e5fa46cc60bb39f1ea2ec9fab` |
| passphrase | `C89l8oqXaTa997+mU79acx+BGgUNf6YPtMoPU9ODyEc=` |

**The profile list.** The home profile's `COMPASS_SPACES` also has one
`CompassProfile {uuid, name, avatar, home}` entity per profile of the chain
(`CompassSpacesSpecifics` oneof field 3; `avatar` is Chromium's default
avatar icon index + 1; client tag = the UUID). The holder asks the home
profile's `CompassSyncService` for this device's profiles
(`GetLocalChainProfiles()`), and hands it every profile entity after a
remote change (`OnChainProfiles()`). Rules:

- Names and avatars follow the chain: the local profile with a chain
  profile's UUID takes its name and avatar (no echo: it's done while
  applying). A rename here commits.
- Profiles are never deleted through sync: deleting one stays on its
  device, and a device without a profile doesn't drop it from the list.
- One home UUID: a home that never published its own takes the chain's (a
  device joining by recovery code); two published ones (two devices joining
  at once) keep the smallest, and the other's entity is deleted.
- A profile of the chain this device doesn't have is **created**
  (`MissingProfiles`, `CompassProfileCreator`) and listed in the switcher,
  which keeps it loaded; its account then fills it (its starting "Home"
  gives way to the account's spaces, as at any initial merge). The device's
  own new profiles join the chain as new profiles (they sync after a
  restart, below). `compass.sync.profile_dirs` on the home remembers which
  directories are the chain's profiles, so one that isn't loaded (no space
  in the switcher) isn't created again.

**Restart behaviour.** `SyncServiceImpl` reads `kEnableLocalSyncBackend`
once, when the profile loads (route 2's mode), so a profile already loaded
when it joins the chain syncs from the next launch, as in slice 2: joining
or starting the chain (with every listed profile) needs one restart, and a
profile made locally while syncing ("New profile…") syncs after the next.
Profiles **created by sync** don't wait: the creator writes the new
directory's `Preferences` before the profile loads (the seed as the home
stores it, server, UUID, `profile_account`, `sync.enable_local_sync_backend`
and its directory), then `ProfileManager::CreateProfileAsync()` loads it,
so its sync service starts in local-sync mode against its account. So B
joining by recovery code restarts once and then gets Work, already
syncing. Ruled out: unloading and reloading a profile to rebuild its sync
service (the profile's prefs commit and the new instance's read race on
different sequences, and it needs every keep-alive gone), a pref store hook
before keyed services (no hook between prefs loading and
`CreateBrowserContextServices()` without a patch). The cost of pre-written
prefs: the profile isn't "new" to Chromium (`IsNewProfile()` reads the
Preferences file's existence), which only affects first-run niceties. If
the prefs didn't take, the home puts it in the chain the normal way and it
syncs after a restart.

**Joining with profiles (the mapping step).** The joining profile becomes
the home here, taking the chain's home UUID. The device's other listed
profiles are mapped to the chain's: each takes over one (its UUID, so its
account; the user's choice) or joins as a new one. Default: the first
untaken chain profile with the same name (ASCII case and outer spaces
ignored), else new. Chain profiles nobody takes are created after the
restart.

- Pairing: message 4 now carries the list, `"profiles": [{"uuid", "name",
  "avatar" (if set), "home": true (home only)}]` (`ChainProfilesToValue()`,
  the host's `SetProfiles()`, the joiner's `profiles()`). With other
  profiles here, the join waits for the mapping before saving anything:
  pairing says "Choose how this Mac's other profiles sync, then Continue."
  and the page shows one row per local profile.
- Recovery code: the list isn't known before the first sync, so the
  joining profile becomes the home at once and, if other profiles exist,
  `compass.sync.profiles_pending` holds them back; once the home's first
  sync brings the list, the page shows the same mapping (status "Choose how
  this Mac's other profiles sync.") and confirming puts them in the chain
  (they sync after a restart) and creates the missing ones (at once).
- The logic is `CompassSyncService::GetProfileMapping()` /
  `SetProfileMapping()` behind `SyncController` (out-of-line defaults in
  `sync_controller.cc`, so other controllers needn't change), the page
  handler's `GetProfileMapping()` / `SetProfileMapping()` mojo calls, and a
  small section in `compass_sync.{html,ts}`. Mapping itself is pure
  functions (`profile_list.h`): `DefaultProfileMapping()`,
  `IsValidProfileMapping()`, `ProfilesToCreate()`.
- Not offered: mapping the joining profile to anything but the home, or
  leaving a chain profile out on this device.

**Spaces follow their profile.** Each profile's bridge syncs its own
`SpaceList` into its own account, which slice 3's per-profile bridges
already did; what's new:

- `MoveSpaceToProfile()` keeps the space's ID (and so its place in the
  switcher: `SpaceDirectory` no longer drops an ID another profile still
  has). The old profile's bridge deletes the space and its nodes from its
  account; the new one's creates them under the same IDs (a re-create over
  tombstones is a revive on the server, sync.md "Server").
- On another device the two changes come in either order. Create first:
  the new profile's bridge finds the space in the old profile here and
  moves it over with its entries (`Delegate::AdoptSpace()` →
  `MoveSpaceToProfile()`), then applies the entities to it; the old
  profile's next pass deletes it from its account (the same deletes). Delete
  first: the space goes, then comes back in the new profile, without its
  live tabs.
- `CompassSpace.profile_uuid` is now set (informational).

**The browser-wide order across accounts.** Positions are still
`UniquePosition`s, but a bridge now assigns its spaces' positions over the
whole switcher (`Delegate::GetDirectoryOrder()`): every listed space in
order, with the positions the other profiles' bridges synced for theirs
(a browser-wide registry fed by `OnSpacePositions()`), so its own fall
between theirs and compare across accounts. After a remote change, the
switcher sorts the spaces with known positions by them, in their slots
(`SortDirectoryByPositions()`), before the bridge's own merge pass
computes positions from it. A switcher change schedules a pass in every
profile's bridge (a space of another profile moving can change this one's
neighbours).

**Leaving** leaves with every profile of the device (each profile's
seed and account flag go; their data stays).

**Logs** (for the steps): the `state=` lines end their first line with
`profile=<directory>`, and "passphrase in place, data types start
(<directory>)", "chain … for <directory> (home profile | profile <uuid>)",
"the chain has N profile(s)", "profile X joins the chain as <uuid> (after a
restart)", "creating profile …" (vmodule `compass_profile_creator`) say
which.

**Patches.** Only `components-sync-protocol-proto_visitors.h.patch`
changes (the `CompassProfile` visitor). No new patch.

**Tests** (targeted filter in the steps doc): `ChainTest.*` (vectors),
`ChainProfilesTest.*`, `ProfileMappingTest.*`, `MissingProfilesTest.*`
(fake creator), `PairingTest.ProfilesTravelWithTheSeed`,
`CompassSpacesSyncProfilesTest.*` and `All/CompassSpacesSyncMoveTest.*`
(two devices × two profiles: two bridges and two in-memory accounts per
device; list round trip, home UUID rules, a space moved Work → Personal in
both arrival orders, interleaved positions),
`CompassSyncPageHandlerMappingTest.*`. `CompassSyncService`'s profile code
(enrolling, creating, renaming, the mapping step) needs real profiles and
has no unit test; the MacBook run covers it.

**Known limits.**

- A profile with no space in the switcher isn't listed, so not kept loaded:
  its sync runs only while something else loads it. Moving a profile's last
  space away unlists it on both devices (each adds a fresh "Home" to the
  unlisted profile, which its account then carries).
- Names follow the chain, so mapping "Job" to the chain's "Work" renames
  it "Work" here.
- Profiles made while syncing sync after a restart; the page of that
  profile says so.
- Deleting a profile stays on its device: the home remembers it
  (`compass.sync.profiles_removed`) and doesn't create it again, other
  devices keep theirs, and the chain keeps listing it. A device that never
  had it still creates it. (A "deleted" flag on `CompassProfile` is the
  follow-up if profiles should be deletable everywhere.)
- Two devices that both hold the home role for a moment (a race at join)
  converge on one home UUID; spaces get re-committed once with the new
  `profile_uuid`.

### Change notifications

Chromium learns about remote changes through FCM invalidations, which need
Google's keys, so they're off. First slices: the server sets a short poll
interval (30 s, go-sync's default, as slice 1 ships it) through
`ClientCommand`, which the engine honours as is, and Compass calls
`SyncService::TriggerRefresh()` (public, no patch) when a window gains focus
and when the Mac wakes. Later (slice 6): a long-lived SSE/WebSocket from
`CompassSyncNotifier` to the server, which calls `TriggerRefresh(types)`
the moment another device commits; polling then goes back to hours.

## The `COMPASS_SPACES` data type

One entity per space and per sidebar node; client tag = the existing UUID
(`SidebarNode::id()`, already stable and saved). Specifics
(`compass_spaces_specifics.proto`):

```proto
message CompassSpacesSpecifics {
  oneof entity {
    CompassSpace space = 1;
    CompassNode node = 2;
  }
}
message CompassSpace {
  string uuid = 1;
  string name = 2;
  int32 color = 3;              // SpaceColor enum value
  string icon = 4;
  bytes directory_position = 5; // UniquePosition in the browser-wide order
  string profile_uuid = 6;      // see "Profiles"
  int64 archive_after_seconds = 7;
}
message CompassNode {
  string uuid = 1;
  string space_uuid = 2;
  string parent_uuid = 3;       // empty = the root of `section`
  enum Section { PINNED = 0; TODAY = 1; FAVORITES = 2; ARCHIVE = 3; }
  Section section = 4;          // FAVORITES: space_uuid empty
  bytes position = 5;           // UniquePosition among siblings
  enum Type { TAB = 0; FOLDER = 1; SPLIT = 2; }
  Type type = 6;
  string url = 7;
  string title = 8;
  string custom_title = 9;
  bool collapsed = 10;          // folders
  int64 last_active_time = 11;  // Today: drives auto-archive
  int64 archived_time = 12;     // ARCHIVE only
}
```

Order uses Chromium's `UniquePosition` (as bookmarks do), so a move is one
entity update, and concurrent inserts in the same folder both survive.

`CompassSpacesSyncBridge` (`browser/sync/`, a
`DataTypeSyncBridge`, one per profile, owned by `SpaceService`) observes
`SpaceList`/`SpaceModel` and turns local changes into `Put`/`Delete` on the
change processor; remote changes are applied through the model's normal
methods with an "origin: sync" flag so the bridge doesn't echo them.
Metadata (`DataTypeStore`, LevelDB in the profile) is separate from
`Compass Sidebar.json`, which stays the source of truth for the tree.

Applying remote changes keeps `SpaceModel::CanMove`'s invariants:

- **Unknown parent** (child arrived before its folder): park it at the end
  of its section's root, move it when the parent arrives.
- **Cycle** (A moved into B on one Mac, B into A on the other): the second
  move to arrive is applied to the section's root instead.
- **Invalid placement** (folder in a split, split in a split): the section's root.
- **Conflicts**: per entity, the last commit to reach the server wins
  (engine default). A rename on one Mac and a move on the other of the same
  node: the later write wins both fields. Acceptable; field-level merge
  later if it bites.
- **Deletes**: removing an entry or space deletes it everywhere. Pinning
  and unpinning are moves between sections (same entity). Deleting a folder
  deletes its subtree (one tombstone per node).
- **Live tabs**: a remote URL change of an unloaded entry just updates it;
  of a live one, it doesn't navigate (see below).

### Today across devices

Today changes far more often than Pinned (every navigation changes a URL
and title), and its entries are often live on one Mac and unloaded on the
other. Rules:

- **What a Today entry means elsewhere**: a tab open on Mac A shows on
  Mac B as an unloaded entry; clicking it loads the URL there (a new
  `WebContents`; page state doesn't travel). Both Macs can have it live.
- **Commit throttling**: the bridge coalesces Today changes per entry and
  commits at most every 10 s (title/URL churn, redirects); structure
  changes (new, close, move, pin) go at once. Applying a remote change never
  commits it back.
- **Navigation on two Macs**: if the entry is live on both and both
  navigate, the last commit wins the entity; the other Mac's live tab keeps
  its page and takes the entity back on its next navigation. No ping-pong:
  only real navigations commit.
- **Closing**: closing an entry on one Mac closes it everywhere. On a Mac
  where it's the shown tab of a focused window, the tab stays open and the
  entry is re-created (the user is looking at it); elsewhere its tab closes.
- **Tabs closed because a window closed or the app quit** stay as unloaded
  entries (as now, D14), so quitting Compass on one Mac doesn't empty Today
  on the others.
- **Restored tabs** (D14) take over entries by URL, synced entries included.
- **New tab page and blank tabs** stay local (they're not saved now either).
- **Auto-archive**: every Mac runs its own timer on the synced
  `last_active_time` (the max over devices, since each commits its own
  activity). Archiving is a move to `ARCHIVE`; two Macs archiving the same
  entry make the same change, so it's harmless.
- **Archive cap**: entries archived more than 30 days ago are deleted from
  sync (kept locally) so the archive can't grow the chain without bound.

**First sync on a device that already has spaces** (decided: merge): union.
The joining device's spaces are added after the chain's; its Today and
favorites are merged into the matching ones (favorites by URL, so the same
site isn't there twice). Entries imported from
Arc get UUIDs derived from Arc's IDs (UUIDv5 of the Arc item ID), so
importing the same Arc data on two Macs merges instead of doubling (affects
arc-import.md, slice 2+).

### Patches for a new data type

Chromium has no plug-in point for types, so these are patches (small, but
every rebase touches them):

- `components/sync/base/data_type.h/.cc`: enum value, info table, root tag,
  histogram suffix.
- `components/sync/protocol/entity_specifics.proto` (field) +
  `compass_spaces_specifics.proto` (new file; it can live in
  `//compass` with the proto `BUILD.gn` importing it), `proto_visitors.h`,
  `proto_value_conversions`.
- `components/sync/base/user_selectable_type.h/.cc` (a "Spaces" toggle).
- `chrome/browser/sync/chrome_sync_controller_builder.cc` (register the
  controller, from `compass::` code).
- `tools/metrics/histograms/metadata/sync/enums.xml` (the type's histogram
  value; `DataTypeForHistograms` must stay in sync or tests fail).

## Profiles

Chromium sync is per profile; Compass spaces are browser-wide and each
belongs to a profile (D17). Plan:

- Each profile syncs to its own server account, derived from the chain
  seed plus the profile's sync UUID: `HKDF(seed, "compass-sync-auth:" +
  profile_uuid)`. Work's passwords stay in Work on every Mac.
- The profile that joined first (the "home" profile) also carries the list
  of profiles (`CompassProfile { uuid, name, avatar }` entities in its
  `COMPASS_SPACES`). A device that sees an unknown profile creates it
  (empty, then it fills from its own account). At join time the user can
  map "Work on the other Mac" to an existing local profile instead.
- A space's entities live in its profile's account. Moving a space to
  another profile = delete in one account, create (same UUIDs) in the other.
- The browser-wide order is `directory_position`, comparable across
  accounts because `UniquePosition`s are plain sortable bytes.

Slices 1–4 sync **one** profile (the one the user turns sync on in);
multi-profile is slice 5, built as "Slice 5 notes" says (the plan above
holds; the passphrase is per profile too).

## Server (`server/` in this repo, Go)

- Start from **brave/go-sync** (MPL-2.0: fine for a fork; modified files
  stay MPL). It already implements the protocol (`GetUpdates`, `Commit`,
  progress markers, `ClientCommand`, quotas).
- Replace its DynamoDB + Redis storage with **SQLite** (one file, one
  binary, a Docker image) so self-hosting is `docker run`. Postgres later
  if a shared instance needs it.
- Replace its auth with the Ed25519 token above.
- Limits per chain: 100k entities, 200 MB, rate limit per IP and per chain.
- Endpoints: `POST /v1/command/` (protocol), `GET /v1/events` (SSE, slice 6),
  `GET /healthz`.
- Self-hosted only for now (decided): there is no Compass-run instance, the
  app's server URL is empty until the user sets one. Ship a
  `docker-compose.yml` with the server and a Caddy front for HTTPS, and a
  short `server/README.md` (VPS, home server behind Tailscale).
- Create over a tombstone: a commit of a new entity (version 0) whose
  client tag belongs to a deleted entity of the same type revives that
  entity: same server ID, new version, the create's content, undeleted
  (Chromium's servers and LoopbackServer do the same; a saved-again
  password or a space put back under its old ID). Over a live entity it is
  a CONFLICT. go-sync instead stored a second entity with a new ID next to
  the tombstone (and answered CONFLICT forever if a delete had left the
  tag item behind). History doesn't use tag items (its ID is the tag), so
  it's unaffected. Client workarounds that re-create under new IDs keep
  working.

## UI

Upstream's sync settings are replaced by a page of our own,
`chrome://compass-sync` (WebUI, like the sidebar):

- Off: "Start sync" (asks for the server URL, makes a chain, shows the
  recovery code once with "I saved it") and "I have another device"
  (server URL + pairing code; "Use a recovery code" under it).
- On: "Add a device" (shows the pairing code and waits, then
  Allow/Deny), the recovery code (behind Touch ID/password via
  `device_reauth`), devices (from `DEVICE_INFO`), toggles per type, server
  URL, "Leave sync" (keeps local data), "Reset sync".
- Status in the header's app menu: last synced, errors.
- Command bar: "Tabs on <device>" from `SESSIONS` (`SessionSyncService`
  open tabs), opened as Today entries.

## Slices

0. **Spike**: done 2026-10-31, route 2 chosen ("Slice 0 findings"). Was:
   (one session, throwaway branch). Run go-sync locally
   (Docker, its own DynamoDB-local), start two Compass instances with
   separate `--user-data-dir`s and `--sync-url=http://localhost:8295/v2`,
   try auth options 1 and 2 with a hard-coded token, and sync a bookmark
   and a password between them. Answer: which auth route; whether 156
   still runs sync-the-feature on desktop (`ConsentLevel::kSync`,
   `IsSyncFeatureEnabled()`) or only transport mode with account stores
   (that changes passwords/bookmarks behaviour: separate account store);
   which types start and which fail without Google; that nothing calls
   Google endpoints (watch the network log). Write the findings here.
1. **Server, minimal** (done 2026-10-31, merged into `sync-main`): go-sync fork in `server/`, SQLite storage, Ed25519
   auth, pairing slots and relay, Docker image, Go tests. A `compass sync-server` CLI command runs it
   locally for development.
2. **Chain + Chromium types** (done 2026-10-31, "Slice 2 run on the
   MacBook"; History moved to 2b): seed, recovery code, pairing (SPAKE2,
   unit tests for wrong code and expiry), HKDF derivations, the auth
   route from slice 0, server URL pref, custom passphrase, Google-only types
   off (through `compass::RegisterFeatureOverrides()` where a feature flag
   exists, patches otherwise), poll + `TriggerRefresh` on focus/wake.
   Minimal `chrome://compass-sync` (start, add a device, join by pairing
   or recovery code, leave). Two-client
   integration test on `FakeServer` for passwords.
2b. **History** (slice 2b, "Slice 2b notes"): HISTORY and delete
   directives under local sync, encrypted.
3. **`COMPASS_SPACES` type** (done 2026-10-31, MacBook run passed, see "Slice 3 notes"; the two-client
   integration test is replaced by multi-bridge unit tests and the MacBook
   run): patches, proto, bridge, apply rules above,
   one profile. Unit tests with `MockDataTypeLocalChangeProcessor`;
   integration tests `TwoClientCompassSpacesSyncTest` (Chromium's
   `SyncTest` with two profiles on a `FakeServer`): create, rename, move,
   concurrent moves into each other (cycle), delete folder, favorites,
   merge at join. Pinned, favorites and collapse state only.
3b. **Today and the archive**: sections `TODAY`/`ARCHIVE`, commit
   throttling, closing rules, auto-archive on synced times, 30-day cap.
   Two-client tests: open on A shows unloaded on B, close on B closes on A
   (and the focused-tab exception), quitting A keeps B's Today, both
   navigate the same entry.
4. **UI**: full settings page, devices, type toggles, status in the app
   menu, "Tabs on other devices" in the command bar.
5. **Profiles** (on `sync/slice-5-profiles`, see "Slice 5 notes"):
   per-profile accounts, profile list, mapping at join, moving spaces
   between profiles.
6. **Push**: SSE from the server, `CompassSyncNotifier`, long poll
   interval.
7. **Hardening**: quota errors shown, key rotation ("Reset sync"), server
   backups, and a real check on two Macs (this one and the MacBook).

Each slice is one or two sessions. Slices 1 and 2 can run in parallel
after slice 0.

## Testing

- Chromium's `FakeServer` stands in for the server in browser/integration
  tests (two clients in one test process), so Compass tests never need the
  Go server. Run only the sync suites touched (memory: targeted tests).
- Go tests for the server, plus one end-to-end script: Go server in
  Docker + two Compass instances (two user data dirs, headless) syncing a
  pinned entry, run by hand per slice, not on every build.
- `chrome://sync-internals` stays available for debugging (traffic dump,
  entity browser).

## Decisions

Decided with the user (2026-10-31):

1. **Server**: self-hostable, and since 2026-10-31 also a public instance
   at `https://sync.example.org/v2` (user's request), on the the sync server
   server: `/opt/compass-sync` (docker compose, image built from
   `server/` copied to `./src`, SQLite in `./data`, bound to
   `127.0.0.1:8295`), behind the host's Caddy
   (`/etc/caddy/sites/sync.example.org.caddy`). Update: rsync
   `server/` to `/opt/compass-sync/src`, then `docker compose up -d
   --build`. Everything on it is encrypted on the devices; the test server
   on the Mac mini (`192.168.1.20:8295`) stays for development.
2. **What syncs**: everything that can, Today and the archive included.
3. **First sync on a device with its own spaces**: merge.
4. **No accounts**: a sync chain. Devices join with a short pairing code
   from a device already syncing; a 12-word recovery code is the fallback
   (24 words was too long).

## Risks

- **Rebase cost**: the data-type patches touch tables that upstream edits
  often (`data_type.h`, the histogram enum). Small but frequent conflicts.
- **Upstream moving away from sync-the-feature** on desktop (transport mode
  + account stores). Slice 0 checks where 156 is; Brave's tree is the
  reference for how they keep up.
- **Protocol drift**: newer Chromium may send fields go-sync doesn't
  know. The server should store specifics opaquely (it can't read them
  anyway) and only interpret envelope fields.
- **No revocation**: a stolen Mac with the seed can read new data until
  "Reset sync".
