# Sync between devices

Status: planned (2026-10-31), decisions taken (see "Decisions"), nothing built. Based on Chromium 156.0.8078.48.
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

Chromium types, on by default: `PASSWORDS`, `HISTORY`, `AUTOFILL` (and
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
- **Auth to the server**: the access token is
  `base64(pubkey || timestamp || sig)`, the signature over
  `"compass-sync" || server_host || timestamp`. The server checks the
  signature and a ±10 min window. No passwords, no email.
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

Server side: `POST /v1/pair` (open, signed), `POST /v1/pair/{slot}/{msg}`
and `GET` with long poll to relay, slots deleted on use or expiry, at most
one open slot per chain, join attempts rate-limited per IP.

"Use a recovery code" on B is the fallback: type the 12 words (BIP39
autocomplete, checksum checked as you type).

### Plugging it into Chromium

The engine wants a signed-in account (`SyncAuthManager` gets an OAuth token
from `IdentityManager`). Two ways, picked by slice 0:

1. **Synthesized account (preferred)**: `SyncAuthManager` (patch or
   `chromium_src` override) returns a `SyncAccountInfo` with
   `gaia = "compass:" + hex(pubkey[0..16])` and our signed token from
   `RequestAccessToken()`, and Compass sets that account as primary in
   `IdentityManager` without Gaia (accounts mutator, no cookies). Brave
   does this. Keeps account-keyed prefs (passphrase bootstrap token,
   selected types per account) working.
2. **Local-sync path**: `kEnableLocalSyncBackend` already runs the engine
   with no identity at all, but `EngineComponentsFactoryImpl` then builds a
   `LoopbackConnectionManager`; we'd patch it to build a network connection
   with our token instead. Fewer identity patches, but local sync limits
   types (`LocalSyncSupportedTypes()`) and can't keep a custom passphrase
   bootstrap token (no Gaia ID, `SetEncryptionBootstrapToken()`), so the
   passphrase would be asked again every launch unless patched.

Either way: the server URL comes from pref `compass.sync.server_url`
(hook in `GetSyncServiceURL()`; `--sync-url` keeps working), and Google
sign-in UI and sync promos stay hidden.

### Change notifications

Chromium learns about remote changes through FCM invalidations, which need
Google's keys, so they're off. First slices: the server sets a short poll
interval (60 s) through `ClientCommand`, and Compass calls
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
multi-profile is slice 5.

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

0. **Spike** (one session, throwaway branch). Run go-sync locally
   (Docker, its own DynamoDB-local), start two Compass instances with
   separate `--user-data-dir`s and `--sync-url=http://localhost:8295/v2`,
   try auth options 1 and 2 with a hard-coded token, and sync a bookmark
   and a password between them. Answer: which auth route; whether 156
   still runs sync-the-feature on desktop (`ConsentLevel::kSync`,
   `IsSyncFeatureEnabled()`) or only transport mode with account stores
   (that changes passwords/bookmarks behaviour: separate account store);
   which types start and which fail without Google; that nothing calls
   Google endpoints (watch the network log). Write the findings here.
1. **Server, minimal**: go-sync fork in `server/`, SQLite storage, Ed25519
   auth, pairing slots and relay, Docker image, Go tests. A `compass sync-server` CLI command runs it
   locally for development.
2. **Chain + Chromium types**: seed, recovery code, pairing (SPAKE2,
   unit tests for wrong code and expiry), HKDF derivations, the auth
   route from slice 0, server URL pref, custom passphrase, Google-only types
   off (through `compass::RegisterFeatureOverrides()` where a feature flag
   exists, patches otherwise), poll + `TriggerRefresh` on focus/wake.
   Minimal `chrome://compass-sync` (start, add a device, join by pairing
   or recovery code, leave). Two-client
   integration test on `FakeServer` for passwords.
3. **`COMPASS_SPACES` type**: patches, proto, bridge, apply rules above,
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
5. **Profiles**: per-profile accounts, profile list, mapping at join,
   moving spaces between profiles.
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

1. **Server**: self-hosted only for now. A public instance may come later.
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
