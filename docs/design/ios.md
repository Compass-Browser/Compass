# Compass for iOS

Status: M0 (scaffold) done. Milestone notes are at the end ("Milestone notes").

**Working on it:**

- `tools/compass ios test [--filter X]` runs CompassKit's tests on the Mac.
- `tools/compass ios build` builds the app for the simulator.
- `tools/compass ios sim [--screenshot f.png]` boots a simulator headless (no Simulator.app window), installs and launches the app.
- `tools/compass ios gen` regenerates `Compass.xcodeproj` from `ios/project.yml`.
- The first run fetches BoringSSL (`ios/scripts/fetch_boringssl.sh`).
- `ios/scripts/gen_protos.sh <chromium src>` regenerates the protos after a Chromium bump.
- Tools: Xcode 27+, plus `brew install xcodegen swift-protobuf` for gen and protos only.

## Context

Compass is an Arc-style Chromium fork for desktop. Its sync stack (on `origin/sync-main`, design in `docs/design/sync.md`) has these parts:

- a go-sync fork in `server/`, with the public instance at https://sync.example.org/v2;
- Chromium's sync protocol, every type encrypted with a Nigori custom passphrase;
- an account-less "sync chain": a 16-byte seed, HKDF → Ed25519 auth key plus a passphrase, a 12-word BIP39 recovery code, and SPAKE2 pairing codes;
- a new `COMPASS_SPACES` type for spaces, pinned trees, favorites, Today and the archive.

ARCHITECTURE.md lists mobile as "later". The goal now is an iPhone browser that:

- uses WebKit (WKWebView, the only engine Apple allows);
- joins the same chain as the desktop;
- shows the same spaces and sidebar in an Arc-style phone UI.

**Decisions (user, 2026-11-01):**

- **Foundation:** a native Swift app (SwiftUI + WKWebView). It has its own Swift sync client and no Chromium for iOS.
- **v1 sync:** spaces and sidebar, passwords with an AutoFill extension, history, open tabs and the device list.
- **Target:** iPhone first, shipped through TestFlight on a paid Apple Developer account. iPad comes later.

Toolchain on the Mac mini: Xcode 27.1 with the iOS 27.1 SDK and simulators. The app builds in minutes, so it doesn't compete with the Chromium builds (`-j 7` rule).

## Repo layout

Everything lives under `ios/` in this repo. The protocol, test vectors and server stay shared with the desktop.

```
ios/
  CompassKit/                 Swift package, tested with `swift test` on the Mac (no simulator)
    Sources/CCryptoBoringSSL/ swift-crypto's prefixed BoringSSL, fetched (gitignored)
    Sources/CompassCrypto/    chain, BIP39, HKDF, Ed25519 token, Nigori, SPAKE2 pairing
    Sources/SyncProto/        swift-protobuf output of Chromium's sync protos + compass_spaces_specifics.proto
    Sources/CompassSync/      engine: connection, GetUpdates/Commit, Nigori, per-type bridges, SSE notifier
    Sources/CompassModel/     Space/SidebarNode tree (Swift port of SpaceModel invariants), GRDB store
    Tests/…                   vectors from sync.md, chain_unittest.cc, bip39_unittest.cc, pairing_unittest.cc
  App/                        SwiftUI app target "Compass"
  AutoFill/                   Credential Provider extension target
  Compass.xcodeproj
  project.yml                 XcodeGen spec (Compass.xcodeproj is generated from it, and committed)
  scripts/fetch_boringssl.sh  copies Sources/CCryptoBoringSSL from swift-crypto at a pinned tag
  scripts/gen_protos.sh       copies components/sync/protocol/*.proto from the checkout at config/chromium_version, runs protoc-gen-swift
docs/design/ios.md            design doc (this plan, kept current like sync.md)
```

The `tools/compass` CLI gets `compass ios build|test|sim` subcommands next to the existing ones in `tools/compass_tool/`.

**Dependencies (SwiftPM):**

- swift-protobuf;
- GRDB (SQLite with FTS5, for history search and sync metadata);
- BoringSSL, as swift-crypto vendors it: every symbol is prefixed `CCryptoBoringSSL_` and it's laid out for SwiftPM. swift-crypto doesn't export it as a product, so `fetch_boringssl.sh` copies it at a pinned tag (5.0.3) instead of us committing 13 MB of C. It gives the same SPAKE2 (`spake25519.cc`), scrypt (`EVP_PBE_scrypt`) and deterministic Ed25519 as the desktop, so the desktop test vectors pass byte for byte. CryptoKit's Ed25519 is randomized, and CryptoKit has no scrypt or SPAKE2. (Building from Chromium's `third_party/boringssl` would need CMake and an xcframework; this needs neither.)
- CryptoKit and CommonCrypto for HKDF, AES-GCM, AES-CBC and HMAC.

## Sync client (CompassSync)

It speaks the same wire protocol as desktop Chromium against `server/`, and needs no server change for v1.

- **Identity.** The phone joins an existing chain in v1. Creating a chain from the phone (writing the Nigori node) is deferred to after v1.
  - Joining by pairing: the phone is side B of the pairing protocol in sync.md "Pairing API". The framing, SPAKE2 names, HKDF infos and the `ad` strings are reused exactly.
  - Joining by recovery code: the 12 words, with BIP39 autocomplete.
  - The seed is stored in the Keychain (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`), in a shared access group so the AutoFill extension can read the passwords.
- **Auth.** Each request carries `Authorization: Bearer base64url(HEX(ts)|HEX(sig)|HEX(pubkey))`, as desktop's `chain.cc` builds it.
- **Engine.** It follows `components/sync/engine` without the parts we don't need:
  - a cache GUID per install, and the store birthday;
  - per-type progress markers, plus a GetUpdates loop (`GU_TRIGGER`/`PERIODIC`);
  - commits in batches;
  - client tag hashes computed as Chromium does (`ClientTagHash::FromUnhashed`: base64(SHA1(serialized-empty-specifics-prefix + tag))), so entities match the desktop's;
  - `ClientCommand` poll intervals honored;
  - on CONFLICT, a re-download and a reapply, with the server winning (the same "last commit wins" as desktop).
  - Sync metadata goes in the GRDB store, kept separate from the model tables.
- **Nigori.**
  - Download `NIGORI`.
  - Derive the key with scrypt (N=8192, the salt from `custom_passphrase_key_derivation_salt`; see `components/sync/model/crypto/nigori.cc`). The passphrase comes from HKDF(seed, "compass-sync-passphrase").
  - Decrypt the keybag. Then decrypt `EncryptedData` (AES-128-CBC + HMAC-SHA256, key name = `Nigori::GetKeyName`) and encrypt with the default key.
  - Passwords also carry their nested `password_specifics.encrypted`.
  - Test vectors: a fixture exported by a small Chromium unit test on the desktop branch. A real chain on the local `compass sync-server` covers the end-to-end case.
- **Types and bridges:**

  | Type | Direction | Notes |
  |---|---|---|
  | NIGORI | read | write later (chain creation from the phone) |
  | DEVICE_INFO | both | own entry, form factor phone, so the desktop's device list shows it |
  | COMPASS_SPACES | both | apply rules from sync.md: unknown parent parked at the section root, cycles to the root, invalid placement to the root, folder delete = subtree tombstones, UniquePosition ordering (Swift port of `components/sync/base/unique_position.cc`), Today commit throttle of 10 s, archive cap of 30 days |
  | PASSWORDS | both | Keychain-backed store shared with the AutoFill extension |
  | HISTORY + HISTORY_DELETE_DIRECTIVES | both | the phone's visits go up; FTS search in the command bar |
  | SESSIONS | both | "Tabs on <Mac>" in the command bar; the phone's Today shows on the desktop through its existing "Tabs on other devices" |

- **Freshness.**
  - In the foreground: SSE from `GET /v2/compass/events?client_id=<cache guid>` (slice 6, `event: changed`, data `{"types":[…]}`) triggers a GetUpdates for those types.
  - On launch and on returning to the foreground: a refresh of every type.
  - In the background: a `BGAppRefreshTask` for a best-effort poll.
  - APNs push (it needs a server-side sender) comes after v1.

## Data model (CompassModel)

This is a Swift port of the desktop model in `browser/sidebar/` (`SidebarNode`, `SpaceModel`, `SpaceModel::CanMove`):

- spaces (name, color, icon, profile UUID, archive delay) and nodes (TAB, FOLDER, SPLIT; PINNED, TODAY, FAVORITES, ARCHIVE);
- the same invariants: folders only in Pinned, splits hold only tabs, and a split of one becomes a tab;
- the same node UUIDs as desktop, which are also the sync client tags.

Live state is separate. A `LiveTabRegistry` maps node → WKWebView, keeps at most 6–8 web views live with LRU unloading, and keeps a snapshot thumbnail for each.

**Profiles.** Each `profile_uuid` gets its own `WKWebsiteDataStore(forIdentifier:)`, so cookies are separated as on desktop. Space ↔ profile follows the synced space field. Multi-account sync per profile waits for desktop slice 5; v1 syncs the chain's home account.

## UI (SwiftUI, Arc-inspired, original implementation)

- **Browser screen.**
  - The web view runs full-bleed, with a slim bottom bar: an address pill, back/forward and a "sidebar" button.
  - The bottom bar hides on scroll down and shows on scroll up.
  - Swiping horizontally on the bar moves between the space's Today/Pinned entries.
- **Sidebar screen** (the home of the app):
  - Spaces sit side by side and are paged by swipe, each tinted with its space color.
  - From top to bottom: a favorites grid (icons), the pinned tree (nested folders that collapse), Today (newest first), and a "+ New tab" button.
  - Long press gives pin/unpin, move to folder or space, rename, archive and delete. Drag reorders entries.
  - The archive opens from the space menu.
- **Command bar** (a sheet from the address pill or "+"):
  - It searches URLs, open entries in every space, pinned entries, history (FTS) and tabs on other devices.
  - The search engine is configurable.
- **Splits** on the phone open as one entry with a pane switcher (segmented control). The desktop-style side-by-side view waits for iPad.
- Auto-archive runs on the synced `last_active_time`, with the same timers as desktop.
- **Onboarding and settings:**
  - "Join with a Mac": scan the QR or type `<slot>-<6 digits>` plus the server URL, defaulting to the the sync server instance. Then "Use a recovery code".
  - Settings: devices, a toggle per type, the server URL, "Leave sync", and the recovery code (behind Face ID).

## Passwords and AutoFill

- **In the app:** a WKUserScript finds login forms and fills from the synced store, and offers to save or update after a submit, which commits to PASSWORDS. Matching follows Chromium's `signon_realm` and PSL rules (simplified: exact realm plus affiliated subdomains).
- **System-wide:** the AutoFill target is an `ASCredentialProviderViewController`.
  - It shares the app group and the Keychain access group, and reads from the same store.
  - The app keeps `ASCredentialIdentityStore` up to date after each sync, so QuickType suggestions work in Safari and other apps.

## Desktop and server changes needed

They are small and go on `sync-main`:

1. The desktop's "Add a device" shows a QR code of `compass-pair://<server host>/<slot>-<digits>` (sync.md already calls for this "for phones later").
2. The desktop's device list and "Tabs on other devices" handle a phone `DEVICE_INFO`/`SESSIONS` entry. Check this; it probably works already.
3. No server change for v1. Server logs during the first phone runs confirm it.

## Milestones (each one a branch `ios/m<n>-…`, merged to main when it passes)

0. **Scaffold.**
   - The `ios/` tree, the Xcode project, the CompassKit package, `build_boringssl.sh`, `gen_protos.sh` and `compass ios` CLI subcommands.
   - An empty WKWebView app that runs in the simulator.
1. **Crypto and chain.**
   - Seed and BIP39, HKDF, deterministic Ed25519 token, all checked against the vectors in sync.md.
   - SPAKE2 pairing as side B, checked against the `PairingKeysTest` vectors.
   - Nigori scrypt and decrypt, checked against a desktop fixture.
2. **Sync engine core.**
   - Connection, GetUpdates and Commit, Nigori, and DEVICE_INFO against the local `compass sync-server` (Podman `compass-sync`).
   - Done when the phone appears in the desktop's device list.
3. **Browser shell and spaces.**
   - The model port, GRDB store and COMPASS_SPACES bridge (read, then write).
   - The sidebar screen, the browser screen and the live-tab registry.
4. **Today, archive, command bar.**
   - Throttled Today commits, auto-archive, profile data stores and splits.
5. **History and sessions.**
   - FTS search and "Tabs on <Mac>".
6. **Passwords and AutoFill.**
   - The in-app autofill script, the save prompt and the extension.
7. **Freshness and polish.**
   - SSE, BGAppRefresh, Face ID for the recovery code, the app icon and branding from `tools/branding`.
   - Then a TestFlight build.

Desktop QR pairing (desktop item 1) runs in parallel with M2.

## Verification

- **Unit tests:** `compass ios test` (= `swift test` in `ios/CompassKit`). It covers the crypto vectors, the UniquePosition port, the model invariants (ported from the SpaceModel unit tests) and the apply rules: unknown parent, cycle, invalid placement, delete subtree. Following the targeted-tests rule, each milestone runs only its own suites.
- **Engine against the server:** an XCTest target starts the repo's Go server on a random port and checks a two-client round trip: a Swift client ↔ another Swift client for each type.
- **Interop with the desktop:**
  1. Pair the simulator, then a device, with a desktop Compass on `sync-main`, on the sync server or the local server.
  2. On the desktop, create a pinned folder and tab, then check them on the phone.
  3. On the phone, open a Today tab, then check it on the desktop as an unloaded entry.
  4. Save a password on the phone, then check it on the desktop.
  5. Check history both ways.
  6. Check that the phone appears in the device list and that "Tabs on iPhone" works.
- **UI:** run in the simulator headless (`xcrun simctl`, with screenshots through `simctl io screenshot`), never in front of the user (the focus rule). Device runs and TestFlight are done by hand by the user.
- **Server:** its Go tests don't change. Watch the logs on the first runs for unknown fields or quota errors.

## Risks

- **Protocol drift and hidden engine behavior:** the Swift engine re-implements parts of `components/sync`. To keep it small: only the types listed, server wins on conflicts, and the desktop's apply code as the reference.
- **Nigori details:** the keybag format and key rotation. The fixture tests catch format mismatches, and "Reset sync" on the desktop must send the phone back to the join screen.
- **iOS background limits:** freshness while the app is in the background is best-effort until APNs.
- **Apple review:** an alternative browser is fine with WKWebView. The default-browser entitlement is an optional request after v1.

## Milestone notes

### M0 (scaffold), 2026-11-01

- **`ios/CompassKit`.** It holds the targets CCryptoBoringSSL, CompassCrypto, SyncProto, CompassModel and CompassSync. `swift test` passes: the chain vectors from sync.md, and checks of each data type's EntitySpecifics field number.
  - The package needs `cxxLanguageStandard: .cxx17`, because BoringSSL's C++ doesn't build with the default.
- **SyncProto.** It holds 96 generated files (2.4 MB) from 156.0.8078.48 with protoc-gen-swift 1.39.0. SwiftProtobuf is pinned to exactly 1.39.0 to match the generator.
  - The Compass files are `components/sync/protocol/compass_spaces_specifics.proto` and the EntitySpecifics patch. `gen_protos.sh` reads them from the tree, or from `origin/sync-main` until that branch is merged.
  - The real proto is the reference, not this doc's sketch. It uses int32 values, not proto enums (section 1 PINNED … 4 ARCHIVE; type 1 TAB, 2 FOLDER, 3 SPLIT; 0 = unset), and `UniquePosition` messages, not bytes.
- **App.** The bundle ID is `it.cvm.compass`. `DEVELOPMENT_TEAM` is empty; set it for device and TestFlight builds.
  - The icon is the desktop's Icon Composer file (`assets/Compass.icon`), used as is.
  - M0 is one WKWebView with an address bar, run in the simulator (iPhone 18 Pro, iOS 27.1).

### M1 (crypto and chain), 2026-11-01

The tests pass on the desktop's vectors and Chromium's own, byte for byte. Everything is in `CompassCrypto`, apart from the pairing state machine and the cryptographer, which are in `CompassSync`.

- **`Chain`.**
  - Seed → auth key, chain ID and passphrase; per-profile auth keys; recovery code ↔ seed.
  - The access token is deterministic Ed25519 through BoringSSL, and the sync.md token vector matches.
- **`Bip39`.** A port of bip39.cc with the same wordlist; its SHA-256 is checked when it's generated.
- **Pairing (side B).**
  - `Spake2` wraps BoringSSL's SPAKE2. `PairingKeys` checks against the PairingKeysTest vectors.
  - `PairingCode` and `PairingInvite` are new. The invite is the QR payload `compass-pair:?server=<url>&code=<slot>-<digits>`, which the desktop has yet to show (desktop item 1).
  - `PairingJoiner` is one async function in place of the desktop's callbacks. `NetworkPairingTransport` uses URLSession.
  - The tests run against an in-memory relay with a scripted Mac (side A): right code, wrong code (the Mac closes with `mismatch`), deny, bad code, unknown slot and used code.
- **`Nigori`.**
  - PBKDF2 and scrypt derivations, the key name, and encrypt/decrypt.
  - Chromium's nigori_unittest goldens pass: the key name, both MAC forms, and the PBKDF2 and scrypt keys.
  - It writes with the MAC over the ciphertext only, as Chromium 156 does by default.
- **`Cryptographer`.**
  - It opens a custom-passphrase NIGORI's keybag (a serialized `EncryptionKeys`) and keeps every key, so older data stays readable. The default key is the keybag's `key_name`.
  - It encrypts and decrypts `EntitySpecifics` with the type's empty field set.
  - It can also make a new chain's NIGORI. The engine tests need that, and so will "Start sync" on the phone.
- **Still open:** no desktop-made NIGORI fixture yet. The M2 interop run (the phone joining a desktop chain) covers it.

### M2 (sync engine core), 2026-11-01

`CompassSync/Engine` holds the engine.

- **`SyncEngine` (an actor).**
  - Downloads every registered type through GetUpdates with progress markers. The first time it uses origin `NEW_CLIENT`, as the server's setup needs.
  - Commits pending changes in batches of 90, or of the server's `max_commit_batch_size`.
  - Honors `ClientCommand`'s poll interval and keeps the store birthday.
- **Entities.** They are keyed by client tag hash (`ClientTagHash`, checked against Chromium's ClientTagHashTest vectors).
- **Encryption.** Everything is encrypted except NIGORI and DEVICE_INFO, as on the desktop. HISTORY is encrypted too (sync.md, "Slice 2b").
- **Nigori.**
  - It reads the chain's NIGORI first and opens it with the passphrase.
  - A chain with an empty NIGORI is `chainNotStarted` for a joining device. A first device (`createChainIfEmpty`) commits a new custom-passphrase NIGORI over the server's root entity.
- **Conflicts.** The remote wins, except that a local edit beats a remote deletion and re-creates the entity (Chromium's default `ResolveConflict`). A commit `CONFLICT` triggers a fresh download, then another commit round (at most 3).
- **Metadata.** `SyncMetadata` (cache GUID, birthday, NIGORI, progress tokens, entity metadata with pending changes) is saved as one binary plist per profile by `FileSyncMetadataStore`. It's small, and GRDB comes with history in M5.
- **Bridges.** A `DataTypeBridge` gives the client tag from specifics, applies remote changes, supplies local data for the initial merge, and gets the cryptographer.
  - `DeviceInfoBridge` writes the phone's entry: tag `DeviceInfo_<cache guid>`, OS iOS, form factor phone, a 24 h pulse.
- **Server tests.** `EngineServerTests` run two Swift clients on one chain against the real Go server (`server/` on sync-main, on a random port with its own SQLite file).
  - They cover: a round trip both ways with updates and a deletion; both devices in DEVICE_INFO; a joiner on an unstarted chain; a wrong passphrase; remote wins; a local edit beating a remote deletion; the initial merge; and no plaintext in the server's database.
  - Run them with `tools/compass ios test --sync-server <binary|build>`. Without it they're skipped.
- **Still open:** the desktop interop run (the phone or simulator joining a desktop chain on sync-main) needs the MacBook. The desktop's QR code is still to do.

### M3 (browser shell and spaces), 2026-11-01

- **CompassModel.** It ports the desktop's sidebar:
  - `SpaceStore` holds the spaces in `UniquePosition` order and the favorites root.
  - `Space` has pinned, Today and archive roots. `SidebarNode` is a root, folder, tab or split.
  - The invariants are the desktop's: folders nest in Pinned **and Today** (the desktop's roots accept both), splits hold at most two tabs and a split of one is dissolved, favorites hold only tabs, and roots don't move.
  - Local edits are reported as a `SpaceStoreChange` (puts, deletes, and Today churn). `applyingRemote` keeps synced changes from echoing.
  - Restoring from the archive makes fresh IDs, as the desktop's `Unarchive` does.
  - The store is saved as one JSON snapshot.
- **UniquePosition.** A byte-for-byte port of Chromium's, including the order-preserving compression. Chromium's insert scenarios and compressed-ordering tests are ported.
- **CompassSpacesBridge.** It follows the desktop bridge's wire rules, summarized from `compass_spaces_sync_bridge.cc` on sync-main:
  - **Tags and positions.**
    - The client tag is the lowercase UUID; `ClientTagHash` gives `C8HbbahHD/9jkO3SUxgcFwf7lVU=` for `0f6d3f4e-1111-4222-8333-444444444444`, as on the desktop.
    - Position suffixes are base64(SHA1(client tag hash)).
  - **Fields.**
    - Times are µs since the Windows epoch. `last_active_time` is only for Today tabs, only moves forward, and only in steps of 60 s or more.
    - Favorites have no `space_uuid`. An empty string or `collapsed=false` is cleared rather than sent.
    - `color`, `archive_after_seconds` and `directory_position` always go on spaces.
  - **Mirror.** Commits start from the mirror of what the server had, so unknown fields survive. Nothing commits until the initial merge (`tracking`, saved with the mirror).
  - **Throttle.** Tab churn (URL, title, last active) goes at most once per entity every 10 s. Structural changes go at once.
  - **Applying remote changes.**
    - Unknown parents are parked at the section root, and nothing is committed for them.
    - Cycles and invalid placements go to the section root, and the fix-up is committed.
    - A remote delete re-roots surviving children. Splits are normalized.
    - Archive entries are section 4, with a split's panes as children.
  - **Initial merge.**
    - Favorites are deduped by URL.
    - An untouched local "Home" (empty Pinned) folds its Today into the chain's first space.
    - Today is deduped by URL.
    - Local spaces go after the chain's.
  - **Not yet:** the desktop's "keep the entry shown in the focused window" rule on a remote close. Here the entry just closes.
- **App.**
  - `AppModel`: store, selection, persistence, and the auto-archive timer.
  - `TabManager`: one WKWebView per live entry, at most 6 (LRU), and a `WKWebsiteDataStore(forIdentifier:)` per profile UUID. Links that open a new window become Today tabs.
  - `SyncController`: Keychain seed and server; joining by pairing code (typed, or from the Mac's QR, `compass-pair:` URL scheme) or by recovery code; cycles on launch, on foreground, 2 s after local edits, and on the server's poll interval.
  - **Screens.**
    - **Sidebar:** the spaces paged by swipe and tinted with their color; favorites grid, pinned tree and Today; context menus for pin, unpin, archive, move to a space, split with, favorite, rename and delete; the archive sheet.
    - **Browser:** a full-bleed web view; a bottom bar with back, address pill, forward and sidebar; a swipe on the bar steps between entries; a pane picker for splits.
    - **Command bar:** search or URL (DuckDuckGo), and switching to open, pinned or favorite entries.
    - **Settings:** sync status, devices, recovery code (behind Face ID or passcode), leave, and joining.
- **Verified.**
  - Two-device sidebar tests on the real server: a full tree (folders, nested folder, split, favorites, two spaces) arrives identical; a reorder, rename, move and delete come back; no echo commits afterwards; archive moves and a restore go across.
  - In the simulator, a debug build launched with `-CompassServer <url> -CompassJoinRecovery "<words>"` joined a chain seeded by `SeedForSimulator` (a test that plays "the Mac") and showed its sidebar and favorites. `-CompassShow <title>` opened an entry in the browser.

### M4 (Today, archive, command bar), 2026-11-01

Most of M4 shipped with M3: the Today throttle, auto-archive on the synced `last_active_time` (a 60 s timer in `AppModel`), the 30-day archive cap, splits with a pane picker, per-profile data stores, and the command bar. This milestone adds the desktop's rule for a remote close of the entry on screen:
- The entry stays and is re-created under fresh UUIDs, so no device re-creates a deleted UUID (`recreateKeptEntries`).
- `onReplaced` moves the live WKWebView and the selection to the new ID, so the page isn't reloaded.
- It's covered by `theShownEntrySurvivesARemoteClose`.

The command bar's history and "Tabs on <Mac>" come with M5.
