# Compass for iOS

A native iPhone and iPad browser (SwiftUI + WKWebView) that joins the same
sync chain as the desktop and shows the same spaces and sidebar. It has its
own Swift sync client and no Chromium for iOS.

**Working on it:**

- `tools/compass ios test [--filter X]` runs CompassKit's tests on the Mac.
- `tools/compass ios build` builds the app for the simulator.
- `tools/compass ios sim [--screenshot f.png]` boots a simulator headless (no Simulator.app window), installs and launches the app.
- `tools/compass ios gen` regenerates `Compass.xcodeproj` from `ios/project.yml`.
- `tools/compass ios archive` makes a Release archive (below, "TestFlight").
- The first run fetches BoringSSL (`ios/scripts/fetch_boringssl.sh`).
- `ios/scripts/gen_protos.sh <chromium src>` regenerates the protos after a Chromium bump.
- Tools: Xcode 27+ with the iOS 27 SDK, plus `brew install xcodegen swift-protobuf` for gen and protos only.

## Context

The desktop's sync stack (design in `docs/design/sync.md`) has these parts:

- a go-sync fork in `server/`, which anyone can run (the project also runs a public instance);
- Chromium's sync protocol, every type encrypted with a Nigori custom passphrase;
- an account-less "sync chain": a 16-byte seed, HKDF → Ed25519 auth key plus a passphrase, a 12-word BIP39 recovery code, and SPAKE2 pairing codes;
- a `COMPASS_SPACES` type for spaces, pinned trees, favorites, Today and the archive.

The phone app:

- uses WebKit (WKWebView, the only engine Apple allows);
- joins the same chain as the desktop;
- shows the same spaces and sidebar in an Arc-style phone UI.

**Decisions:**

- **Foundation:** a native Swift app (SwiftUI + WKWebView), with its own Swift sync client.
- **Sync:** spaces and sidebar, passwords with an AutoFill extension, history, open tabs, the device list, sent tabs and the desktop's extensions list.
- **Target:** iPhone first, shipped through TestFlight. iPad has its own layout (below).

## Repo layout

Everything lives under `ios/`. The protocol, test vectors and server are shared with the desktop.

```
ios/
  CompassKit/                 Swift package, tested with `swift test` on the Mac (no simulator)
    Sources/CCryptoBoringSSL/ swift-crypto's prefixed BoringSSL, fetched (gitignored)
    Sources/CompassCrypto/    chain, BIP39, HKDF, Ed25519 token, Nigori, SPAKE2 pairing
    Sources/SyncProto/        swift-protobuf output of Chromium's sync protos + compass_spaces_specifics.proto
    Sources/CompassSync/      engine: connection, GetUpdates/Commit, Nigori, per-type bridges, SSE notifier
    Sources/CompassModel/     Space/SidebarNode tree (Swift port of SpaceModel invariants), stores
    Sources/CompassClaude/    Claude's pure parts (claude.md)
    Sources/CompassShared/    what the app and its extensions share
    Tests/…                   vectors from sync.md, chain_unittest.cc, bip39_unittest.cc, pairing_unittest.cc
  App/                        SwiftUI app target "Compass"
  AutoFill/                   Credential Provider extension
  PushService/                notification service extension (sent tabs)
  Share/, Widgets/, SafariOpen/, Shared/   system extensions
  UITests/
  Compass.xcodeproj
  project.yml                 XcodeGen spec (Compass.xcodeproj is generated from it, and committed)
  scripts/fetch_boringssl.sh  copies Sources/CCryptoBoringSSL from swift-crypto at a pinned tag
  scripts/gen_protos.sh       copies components/sync/protocol/*.proto from the checkout at config/chromium_version, runs protoc-gen-swift
```

**Dependencies (SwiftPM):**

- swift-protobuf, pinned to exactly the generator's version (1.39.0);
- BoringSSL, as swift-crypto vendors it: every symbol is prefixed `CCryptoBoringSSL_` and it's laid out for SwiftPM. swift-crypto doesn't export it as a product, so `fetch_boringssl.sh` copies it at a pinned tag (5.0.3) instead of us committing 13 MB of C. It gives the same SPAKE2 (`spake25519.cc`), scrypt (`EVP_PBE_scrypt`) and deterministic Ed25519 as the desktop, so the desktop test vectors pass byte for byte. CryptoKit's Ed25519 is randomized, and CryptoKit has no scrypt or SPAKE2. (Building from Chromium's `third_party/boringssl` would need CMake and an xcframework; this needs neither.) The package needs `cxxLanguageStandard: .cxx17`, because BoringSSL's C++ doesn't build with the default.
- CryptoKit and CommonCrypto for HKDF, AES-GCM, AES-CBC and HMAC.
- The system's `SQLite3` (with FTS5) for history.

## Sync client

It speaks the same wire protocol as desktop Chromium against `server/`.

- **Identity.** The phone joins an existing chain; creating a chain from the phone isn't offered.
  - Joining by pairing: the phone is side B of the pairing protocol in sync.md "Pairing API". The framing, SPAKE2 names, HKDF infos and the `ad` strings are reused exactly.
  - Joining by recovery code: the 12 words, with BIP39 autocomplete.
  - The seed is stored in the Keychain (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`), in a shared access group so the extensions can read it.
- **Auth.** Each request carries `Authorization: Bearer base64url(HEX(ts)|HEX(sig)|HEX(pubkey))`, as desktop's `chain.cc` builds it.
- **Types and bridges:**

  | Type | Direction | Notes |
  |---|---|---|
  | NIGORI | read | the phone never writes it |
  | DEVICE_INFO | both | own entry, form factor phone, so the desktop's device list shows it |
  | COMPASS_SPACES | both | the apply rules from sync.md |
  | PASSWORDS | both | Keychain-backed store shared with the AutoFill extension |
  | HISTORY + HISTORY_DELETE_DIRECTIVES | both | the phone's visits go up; FTS search in the command bar |
  | SESSIONS | both | "Tabs on <Mac>" in the command bar; the phone's Today shows on the desktop through its "Tabs on other devices" |
  | SEND_TAB_TO_SELF | both | tabs sent to the phone (send-tab.md) |
  | EXTENSIONS | read | the desktop's extensions, offered for install |

The parts were built as milestones M0–M8, which the code still refers to:
M0 scaffold, M1 crypto and chain, M2 engine core, M3 browser shell and
spaces, M4 Today and archive, M5 history and sessions, M6 passwords and
AutoFill, M7 freshness, M8 per-profile accounts.

### Crypto and chain (M1)

Everything is in `CompassCrypto`, apart from the pairing state machine and the cryptographer, which are in `CompassSync`. The tests pass on the desktop's vectors and Chromium's own, byte for byte.

- **`Chain`.**
  - Seed → auth key, chain ID and passphrase; per-profile auth keys and passphrases; recovery code ↔ seed.
  - The access token is deterministic Ed25519 through BoringSSL, and the sync.md token vector matches.
- **`Bip39`.** A port of bip39.cc with the same wordlist; its SHA-256 is checked when it's generated.
- **Pairing (side B).**
  - `Spake2` wraps BoringSSL's SPAKE2. `PairingKeys` checks against the PairingKeysTest vectors.
  - `PairingCode` and `PairingInvite`. The invite is the QR payload `compass-pair:?server=<escaped server URL>&code=<slot>-<digits>` that the desktop's "Add a device" shows (sync.md, "Pairing QR code").
  - `PairingJoiner` is one async function in place of the desktop's callbacks. `NetworkPairingTransport` uses URLSession.
  - The tests run against an in-memory relay with a scripted Mac (side A): right code, wrong code (the Mac closes with `mismatch`), deny, bad code, unknown slot and used code.
- **`Nigori`.**
  - PBKDF2 and scrypt derivations (scrypt N=8192, the salt from `custom_passphrase_key_derivation_salt`; see `components/sync/model/crypto/nigori.cc`), the key name, and encrypt/decrypt (AES-128-CBC + HMAC-SHA256).
  - Chromium's nigori_unittest goldens pass: the key name, both MAC forms, and the PBKDF2 and scrypt keys.
  - It writes with the MAC over the ciphertext only, as Chromium does by default.
- **`Cryptographer`.**
  - It opens a custom-passphrase NIGORI's keybag (a serialized `EncryptionKeys`) and keeps every key, so older data stays readable. The default key is the keybag's `key_name`.
  - It encrypts and decrypts `EntitySpecifics` with the type's empty field set.
  - It can also make a new chain's NIGORI, which the engine tests need.

### Engine (M2)

`CompassSync/Engine` follows `components/sync/engine` without the parts the phone doesn't need.

- **`SyncEngine` (an actor).**
  - Downloads every registered type through GetUpdates with progress markers. The first time it uses origin `NEW_CLIENT`, as the server's setup needs.
  - Commits pending changes in batches of 90, or of the server's `max_commit_batch_size`.
  - Honors `ClientCommand`'s poll interval and keeps the store birthday and a cache GUID per install.
- **Entities.** They are keyed by client tag hash, computed as Chromium does (`ClientTagHash::FromUnhashed`), checked against Chromium's ClientTagHashTest vectors.
- **Encryption.** Everything is encrypted except NIGORI and DEVICE_INFO, as on the desktop. HISTORY is encrypted too (sync.md, "History"). Passwords also carry their nested `password_specifics.encrypted`.
- **Nigori.** It reads the chain's NIGORI first and opens it with the passphrase. A chain with an empty NIGORI is `chainNotStarted` for a joining device. A first device (`createChainIfEmpty`, tests only) commits a new custom-passphrase NIGORI over the server's root entity.
- **Conflicts.** The remote wins, except that a local edit beats a remote deletion and re-creates the entity (Chromium's default `ResolveConflict`). A commit `CONFLICT` triggers a fresh download, then another commit round (at most 3).
- **Metadata.** `SyncMetadata` (cache GUID, birthday, NIGORI, progress tokens, entity metadata with pending changes) is saved as one binary plist per account by `FileSyncMetadataStore`, separate from the model.
- **Bridges.** A `DataTypeBridge` gives the client tag from specifics, applies remote changes, supplies local data for the initial merge, and gets the cryptographer.
  - `DeviceInfoBridge` writes the phone's entry: tag `DeviceInfo_<cache guid>`, OS iOS, form factor phone, a 24 h pulse, and the send-tab-to-self receiving fields.
- **Server tests.** `EngineServerTests` run two Swift clients on one chain against the real Go server (`server/`, on a random port with its own SQLite file): a round trip both ways with updates and a deletion; both devices in DEVICE_INFO; a joiner on an unstarted chain; a wrong passphrase; remote wins; a local edit beating a remote deletion; the initial merge; and no plaintext in the server's database. Run them with `tools/compass ios test --sync-server <binary|build>`. Without it they're skipped.

### Spaces (M3, M4)

- **CompassModel** ports the desktop's sidebar (`browser/sidebar/`: `SidebarNode`, `SpaceModel`, `SpaceModel::CanMove`):
  - `SpaceStore` holds the spaces in `UniquePosition` order and the favorites root.
  - `Space` has pinned, Today and archive roots, and a theme (below). `SidebarNode` is a root, folder, tab or split. Node UUIDs are the desktop's, and are also the sync client tags.
  - The invariants are the desktop's: folders nest in Pinned and Today (the desktop's roots accept both), splits hold at most four tabs and a split of one is dissolved, favorites hold only tabs, and roots don't move.
  - Local edits are reported as a `SpaceStoreChange` (puts, deletes, and Today churn). `applyingRemote` keeps synced changes from echoing.
  - Restoring from the archive makes fresh IDs, as the desktop's `Unarchive` does.
  - The store is saved as one JSON snapshot.
- **UniquePosition.** A byte-for-byte port of Chromium's, including the order-preserving compression. Chromium's insert scenarios and compressed-ordering tests are ported.
- **The proto.** `compass_spaces_specifics.proto` is the reference. It uses int32 values, not proto enums (section 1 PINNED … 4 ARCHIVE; type 1 TAB, 2 FOLDER, 3 SPLIT; 0 = unset), and `UniquePosition` messages, not bytes.
- **CompassSpacesBridge.** It follows the desktop bridge's wire rules (`compass_spaces_sync_bridge.cc`):
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
    - A remote close of the entry on screen keeps it: it's re-created under fresh UUIDs, so no device re-creates a deleted UUID (`recreateKeptEntries`), and `onReplaced` moves the live WKWebView and the selection to the new ID, so the page isn't reloaded (`theShownEntrySurvivesARemoteClose`).
  - **Initial merge.**
    - Favorites are deduped by URL.
    - An untouched local "Home" (empty Pinned) folds its Today into the chain's first space.
    - Today is deduped by URL.
    - Local spaces go after the chain's.
- **Auto-archive** runs on the synced `last_active_time` (a 60 s timer in `AppModel`), with the desktop's timers and a 30-day archive cap.
- **Tests:** two-device sidebar tests on the real server: a full tree (folders, nested folder, split, favorites, two spaces) arrives identical; a reorder, rename, move and delete come back; no echo commits afterwards; archive moves and a restore go across.

### History and sessions (M5)

- **`HistoryStore`** (CompassModel). SQLite with an FTS5 index over URL and title.
  - A visit is keyed by (device, visit time in Windows-epoch µs).
  - History is kept 90 days.
- **`HistoryBridge`.** It follows Chromium's HistorySyncBridge.
  - One entity per visit, client tag = the visit time as a decimal string.
  - The fields the desktop requires are set: visit time, originator cache GUID, one redirect entry. Page transition is LINK or TYPED, browser type tabbed.
  - Only http(s) pages are synced (Chromium's ShouldSync).
  - Visits from other devices are added; our own echoes are ignored.
  - A later title for the same visit re-commits the same tag, which the server treats as an update.
- **Untracked entities.** The engine keeps no metadata for HISTORY and its delete directives, as Chromium untracks them after a commit (`DataType.tracksEntities`). Every device writes only its own visits, so there's nothing to conflict.
- **`HistoryDeleteDirectivesBridge`.**
  - Received time-range, URL and global-ID directives delete local visits. Their times are Unix-epoch µs; global IDs are visit times.
  - "Clear History" sends a time-range directive with a random 8-character tag, as Chromium does. Directives aren't deleted after use.
- **`SessionsBridge`.**
  - Each device has a header (tag = its cache GUID) and one tab entity per tab (tag `"<guid> <tab node id>"`).
  - The phone publishes its Today tabs as one window, on every cycle. Only changed tabs and the header are committed; tabs that went away are deleted.
  - Tab node IDs per sidebar UUID are saved, so they stay stable across launches.
  - Other devices' sessions untouched for 14 days are hidden.
- **App.** Visits are recorded when a page finishes loading. The command bar shows history matches ("Recently visited" when empty) and "Tabs on <device>". Settings has "Clear History".
- **Server tests:** history both ways, a title update, and engine metadata staying empty; a delete directive from the phone clearing the Mac's history; sessions both ways, a closed tab disappearing, and no re-commit of unchanged tabs.

### Passwords and AutoFill (M6)

- **Wire format.** It follows Chromium's PasswordSyncBridge and data_type_worker.cc:
  - `password.encrypted` holds the serialized `PasswordSpecificsData`, and `encrypted_notes_backup` its notes.
  - The outer `encrypted` field is unused, and no unencrypted metadata is sent with a custom passphrase.
  - `client_only_encrypted_data` stays local.
  - The client tag is `EscapePath(origin spec)|EscapePath(username element)|EscapePath(username)|EscapePath(password element)|EscapePath(signon realm)`. Chromium's client-tag test and base's EscapePath test pass.
  - Saves set `date_created`, `date_last_used` and `date_password_modified` (Windows-epoch µs), `times_used`, scheme 0 (HTML) and type 0. Updates keep the stored data's other fields.
  - "Never for this site" is Chromium's blocklisted entry.
- **Storage.** `KeychainPasswordStore` keeps one item per credential, holding the serialized data. The access group `$(AppIdentifierPrefix)it.cipi1965.compass.shared` is shared with the extension, with a fallback to the app's own Keychain when the build isn't signed with the group (simulator).
- **In pages.**
  - The `PasswordScript` user script runs in its own content world. It reports login forms and submitted credentials (on submit, a button click, Enter, or pagehide) and fills on request.
  - The browser bar shows a key menu when the page has a login form and saved logins match (same realm, or a parent or child host: a simplified form of Chromium's `signon_realm` and PSL rules).
  - A banner offers Save, Update, Never for This Site, or Not Now.
- **Settings → Passwords**, behind Face ID or the passcode.
- **AutoFill extension** (`ios/AutoFill`, an `ASCredentialProviderViewController`).
  - It shows a suggested list for the requesting site and fills from QuickType without UI.
  - The app keeps `ASCredentialIdentityStore` current after every change, so QuickType suggestions work in Safari and other apps. The user turns it on in iOS Settings → Passwords → AutoFill.
  - Device builds need a team with the AutoFill credential provider capability and the shared Keychain group (`App/Compass.entitlements`, `AutoFill/AutoFill.entitlements`).
- **Tests:** a save on the Mac arrives on the phone; an update and a deletion come back; neither the password nor the domain is readable on the server.

### Freshness (M7)

- **`SyncNotifier`.** The server's push stream (sync.md, "Change notifications"): `GET <server>/compass/events?client_id=<cache guid>`, signed like `/command/`.
  - It parses event streams per the HTML spec (LF, CR or CRLF; comments; a 64 KiB line cap).
  - It does a full refresh on every connect, and reconnects with backoff from 1 s up to 60 s.
  - The app holds it only while active; a `changed` event (data `{"types":[…]}`) triggers a cycle 0.3 s later.
  - The server poll (15 min by default) and `BGAppRefreshTask` (`it.cipi1965.compass.refresh`, at least 15 min apart, best effort) cover the rest.
- Cycles also run on launch, on returning to the foreground, and 2 s after local edits.
- **Pairing links.** `compass-pair:` links (the system camera scanning the Mac's QR) open Settings → Join with the server and code filled in, and start joining.
- **Signing in the simulator.** `compass ios build/sim` sign simulator builds ad hoc. Unsigned builds get no application identifier, so the chain's Keychain items don't survive a reinstall.

### M8: per-profile accounts

The phone syncs every profile of the chain, each in its own account, as the desktop does (sync.md, "Profiles"), and shows all their spaces in one sidebar.

- **Keys.** `Chain.passphrase(profileUUID:)` = base64 HKDF(seed, "compass-sync-passphrase:" + uuid), next to `authKey(profileUUID:)`. sync.md's profile account vectors ("Identity": account ID and passphrase) pass in `ChainTests.profileAccountVectors`.
- **Profile list.** `ChainProfile` (CompassSync) mirrors `profile_list.h`. It comes from the home account's `CompassProfile` entities (the home bridge's mirror), or from pairing message 4's `"profiles"` (`PairingJoiner.Result.joined(seed:profiles:)`, parsed like `ChainProfilesFromValue()`).
  - A pairing join starts every account at once. A recovery-code join starts the home account; the others start when its first sync brings the list (`SpaceDirectory.onProfilesChanged`).
  - Home UUID: the entity marked home, the smallest of several (the desktop's rule for two homes). It goes into `SpaceStore.homeProfileUUID`, and the home's spaces carry it in `profile_uuid`, as the desktop's do.
  - The phone never writes, creates or deletes chain profiles.
- **Accounts.** `SyncController.Account` per profile: its own engine, metadata file, cache GUID, NIGORI (its own passphrase), bridges and `SyncNotifier` stream, as each desktop profile runs its own. The home's files keep their original names; the others live in `Profiles/<uuid>/`. A cycle runs the home first, then the others. DEVICE_INFO goes in every account, so each profile's device list on the Mac shows the phone; Settings shows the home's list. The SSE stream is one per account, not multiplexed, which matches the desktop.
- **`SpaceDirectory`** (CompassSync): one `SpaceStore`, one `CompassSpacesBridge` per account, and who owns what.
  - A space belongs to the bridge of its profile UUID; home spaces (empty UUID or the home's) and profiles without an account here go to the home's. Nodes follow their space.
  - **Favorites are per profile**, as on the desktop: `SpaceStore.favorites` is the home's, `favorites(forProfile:)` the others' (root ID `favorites:<uuid>`). A space page shows its profile's favorites.
  - Local changes go to every bridge; each commits only what it owns. After any change, local or remote, `reconcile()` has each bridge give up (delete from its account) what another owns now. A node still waiting for its space isn't given up.
  - **Cross-account order.** The store keeps every profile's spaces in one `UniquePosition` order, so a move puts a space between its neighbours' positions whatever their accounts are. That is what the desktop's `GetDirectoryOrder()` + `SortDirectoryByPositions()` do with a registry; the phone needs neither.
  - **Space moved between profiles** (the desktop's `MoveSpaceToProfile()`). `SpaceStore.setProfile(_:to:)` keeps the IDs: the new account creates the space and its entries, and the old one deletes them through `reconcile()`. Arriving from another device in either order:
    - create first: the new account's bridge adopts the space with its entries (`AdoptSpace()`);
    - delete first: the space goes, then comes back.
  - A remote delete only removes what that bridge owns, so a late delete from the old account doesn't remove an adopted space.
  - Moving a folder to another space re-commits its subtree (the children's `space_uuid` changes).
- **Passwords.** `KeychainPasswordStore(profile:)`: the home keeps the service `it.cipi1965.compass.passwords`, the others use `….<uuid>`. `SavedPassword.profile` and `recordID` (unique across profiles) exist for this.
  - In-page fill and saves use the account of the tab's space.
  - `everyProfile(accessGroup:)` feeds Settings, the `ASCredentialIdentityStore` list and the AutoFill extension.
- **History.** `visits.profile` column (old rows are the home's). Visits go to the tab's account, and a delete directive deletes only its account's visits. "Clear History" sends one per account.
- **Sessions.** Each account publishes the Today tabs of its own spaces. Foreign sessions are kept per account.
- **UX.**
  1. One row of spaces, ordered across profiles as on the desktop. A profile is only an attribute of a space: a small profile label on the space header, only when the chain has more than one profile, and a wider gap in the page dots where the profile changes. No profile switcher.
  2. Only spaces are selected. A new tab goes in the current space, a new space in the current space's profile. The phone never asks for a profile.
  3. "Move to Profile ▸ <names>" in the space's menu (the ⋯ button and a long press on the header). It asks first: "Move "<space>" to <profile>? Its tabs will be signed out of sites, because each profile keeps its own cookies." The space's live tabs are unloaded, so they reload in the new profile's data store. The wire behaviour is the desktop's (above).
  4. The command bar is scoped to the current space's profile: open and pinned entries of that profile's spaces, its favorites (`SpaceStore.tabs(inProfileOf:)`), its account's history (`HistoryStore.search(_:profile:)`) and its account's "Tabs on <device>". Profiles never mix.
  5. The save banner says "Save to <profile>?" when there's more than one profile. The AutoFill extension groups credentials under one header per profile. Names come from a shared Keychain item (`KeychainPasswordStore.saveProfileNames`).
  6. After joining (pairing or recovery code): one summary screen, "Synced N profiles: Personal, Work · M spaces", with Continue (`JoinSummaryView`). Settings → Profiles lists each profile with its number of spaces.
- **Tests:** `SpaceDirectoryTests` (the pairing list, the smallest home, routing by profile, a tab and a space moved across profiles, command bar scoping), `PairingJoinerTests` (the profile list from message 4), and `ProfilesServerTests`, two Swift clients on one chain with two profiles, where "the Mac" writes the `CompassProfile` list and spaces in both accounts: every profile's spaces arrive in cross-account order with per-profile favorites; phone edits go to the right account; a space moved between profiles arrives in both orders and goes back; passwords and history stay in their profile; no names, URLs, passwords or profile UUIDs are readable in the server's database.
- **Limits.**
  - A remote adoption doesn't reload the adopted space's live tabs in the new data store. They change store the next time they load.
  - Devices in Settings come from the home account only.
  - A chain profile the desktop deletes stays listed (sync never deletes profiles, as on the desktop).

## App

- `AppServices` (one per app; below, "Multiple windows") holds the store, live tabs, sync, passwords, extensions, downloads and system bridges. `AppModel` (one per window) holds what the window shows.
- `TabManager`: one WKWebView per live entry, at most 6 (LRU), and a `WKWebsiteDataStore(forIdentifier:)` per profile UUID, so cookies are separated as on desktop. Links that open a new window become Today tabs.
- `SyncController`: Keychain seed and server; joining by pairing code (typed, or from the Mac's QR, `compass-pair:` URL scheme) or by recovery code.
- The app icon is the desktop's Icon Composer file (`assets/Compass.icon`), used as is.

### Screens

- **Browser screen.**
  - The web view runs full-bleed, with a slim bottom bar: back, an address pill, forward and a sidebar button. The bar is neutral: primary-colored icons and a grey address pill, not the system tint.
  - The bottom bar hides on scroll down and shows on scroll up.
  - Swiping horizontally on the bar moves between the space's Today/Pinned entries, as in Safari: the page and the bar follow the finger and the neighbor (its last snapshot, or its icon and title when it isn't loaded, and its bar) slides in beside them. Let go past a third of the screen, or flick past half, and it takes over; otherwise both spring back. Past the space's ends the swipe only gives a little.
  - Swiping up on the bar opens, by where the finger starts: left of the address pill (back, forward) the tab picker, on the pill the sidebar (both follow the finger), right of it (key, new tab, menu) the command bar for a new tab. The pill's frame on screen is measured; thirds of the screen until it is.
  - A long press on the address pill: Reload, Copy URL, Share, Find on Page, and Request Desktop Site / Request Mobile Site, which flips the site's Desktop Site setting (Site Settings) and reloads its tabs.
  - A long press on the bar's … button: Share the shown page (its URL and title, as the menu's Share tile).
  - Splits on the phone open as one entry. A split of two shows both panes, one above the other (side by side in landscape), each under a title strip; the front pane is outlined and the bar acts on it, and a tap on a strip brings that pane to the front. Only the bottom pane runs under the bar. A split of three or four shows one pane at a time under a pane picker (segmented control). `AppModel.showsPanesTogether`.
- **Tab picker** (`TabPicker`): the loaded tabs only, as cards with their snapshot (taken as a tab is left and as the picker opens, in memory), least recently shown at the left. Cards left of the centered one stack behind it; cards to its right lie on top. A tap shows a tab, a flick up closes it (a Today entry goes to the archive, anything else unloads), a drag down closes the picker. One button: + (new tab).
- **Command bar** (a sheet from the address pill or "+"): search or URL (DuckDuckGo by default; configurable), open, pinned and favorite entries, history (FTS) and tabs on other devices.
- **Settings:** sync status, devices, profiles, the recovery code (behind Face ID or passcode), "Leave sync", and joining: "Join with a Mac" (scan the QR or type `<slot>-<6 digits>` plus the server URL, defaulting to the public instance), then "Use a recovery code".
- **Start screen.** Settings → On Launch (`StartScreen`, AppModel.swift): **Sidebar** (default: the sidebar over an empty page) or **Last Tab** (the tab last shown, sidebar down, if it's still open: not closed or archived; else the sidebar). The shown tab and space are kept in `UserDefaults` (`LastShownTab`, `LastShownSpace`) as they change; a launch always opens on the space last shown. Tests: `StartScreenTests`.

### Sidebar

On the iPhone the sidebar is a system sheet over the browser (`.sheet` with `presentationDetents([.medium, .large])` and a drag indicator), not a screen the browser replaces.

- It opens at full height, at launch and from the sidebar button. Swiping it down to half height shows the page; swiping it all the way down closes it. Swiping up on the browser's bottom bar opens it again.
- Picking an entry closes the sheet. With nothing selected, the browser shows an empty page in the space's color.
- The command bar and Settings are presented by whichever view is in front: the sidebar while it's up, the browser otherwise (`AppModel.presented(_:fromSidebar:)`). One view can't present a second sheet over its own.
- Spaces sit side by side and are paged by swipe. From top to bottom: a favorites grid, the pinned tree (nested folders that collapse), Today (newest first). The archive opens from the space menu.
- The long-press menu has pin, unpin, archive, move to a space, split with (another tab), add to split (a tab into a split beside it, or on a split a tab beside it into the split, up to four panes), favorite, rename and delete, and for a pinned tab or favorite its pinned URL ("Use This Page as Pinned URL", "Back to Pinned URL"; closing goes back to it, see sidebar.md).

**Styled as the desktop's.** The look follows the desktop sidebar (`resources/sidebar/app.css`). The tokens are `SidebarStyle` in `Theme.swift`.

- **Background.** The space's tint (`SpaceTint`) is the sheet's `presentationBackground`, like the desktop's SidebarTint.
- **Favorites.** Tiles are 52 pt, four to a row, and a shorter last row's tiles grow to fill it. The shown favorite gets the light pill.
- **Header.** The space's name, its profile label and a round ⋯ menu.
- **Sections.** "PINNED" and "TODAY" headers are small, uppercase and dimmed, as rows rather than sticky List headers.
- **Entries.**
  - Borderless 44 pt rows (the desktop's are 32 px), with a light pill and shadow on the shown entry and a grey fill when pressed.
  - Pinned entries without a live tab are dimmed, as on the desktop.
  - Folders have a rotating chevron. A split's panes share one row.
- **Space switcher.** Each space's icon in the bottom bar (`SpaceIcon`), the shown one in its color and the others dimmed, then "+". The icon is the desktop's `{"symbol"}` or `{"emoji"}`, mapped to SF Symbols, or the space's initial.
- **A stack, not a List.** List rows only fade, and a row's height changes in one jump. So the sidebar is a `LazyVStack` in a ScrollView, and List's built-ins are replaced the desktop's way: Today rows have an × button, and a long press and drag reorders (`.draggable`/`.dropDestination`; dropped on an entry it goes before it, on a folder into it, on a section's header to that section). The long-press menu still works.
- **Motion.** A folder's children slide down from under its row as they fade in, and slide back up on close, in 0.22 s (`SidebarStyle.folder`). The rows below move with them, the chevron turns and the folder icon swaps. Every other change animates too (`SidebarStyle.motion`, keyed on the entries, favorites and the shown entry), including changes synced from other devices. The shown space's icon grows a little with a spring. Rows and tiles shrink slightly when pressed.
- **Desktop-only pages hidden.** Entries that aren't web pages (`compass://`, `chrome://`, `about:`, `file:`…) still sync and stay in the store, but the phone doesn't show them: not in the sidebar, favorites, split panes, the bar's swipe between entries, or the command bar (open entries and "Tabs on <device>"). Folders always show. "Clear" archives only what's shown. The rule is `PhoneVisible` / `SidebarNode.isPhoneVisible` in `Theme.swift`.

### Favicons

- **Loading.** `Favicons` (App/Model) keeps icons by host in memory and in `Caches/Favicons` (`AsyncImage` kept no cache and left a letter when a redraw cancelled its load). Best first:
  1. a loaded page's declared icons (the largest), or its `/favicon.ico` when it declares none;
  2. an icon synced from another device;
  3. a guess: `apple-touch-icon.png`, then `/favicon.ico`.

  Requests use Safari's user agent.
- **Sync.** `CompassFavicon` in COMPASS_SPACES (sync.md, "Favicons"). `CompassSpacesBridge+Favicons`: `offerFavicon` writes a page's icon (64 px PNG, at most 8 KiB) into the account of the tab's space, when an entry there is on the host and the icon beats the account's. `onFavicon` and `syncedFavicons` feed the cache, and `collectFavicons` deletes unused icons after a day. Tests: `FaviconKeyTests` (the UUID vectors) and `FaviconServerTests`.

### Space themes

The desktop's custom colors and gradients (spaces.md, "Space colors" through "Theme editor").

- **Model.** `SpaceTheme` (CompassModel/SpaceTheme.swift) holds any `RGB` color or nil for grey, up to two gradient colors (grey has none), intensity (default 0.31), grain and `SpaceAppearance` (auto, light, dark), clamped like the desktop's setters. `SpaceColor` is the palette, with the desktop's exact RGB values. `stops` gives the three evenly spaced stops `SidebarTint::ToLook()` paints. `SpaceStore.setTheme()`. The snapshot file has `colorRGB`, `gradient`, `intensity`, `grain` and `appearance` next to `color`. Older files read as their palette color.
- **Sync.** `SyncPb_CompassSpace.theme(over:)` and `setTheme()` (SpaceThemeProto.swift) follow compass_spaces_sync_bridge.cc. `color_rgb` counts only while `color` is still its nearest palette index. Every value is compared decoded, so an equivalent encoding isn't rewritten and a remote theme isn't committed back. Defaults are left out, and an unreadable color keeps the space's own.
- **Look.** `SpaceTint(theme:)` paints the stops as one gradient from the top left to the bottom right, at `intensity × 1.1` opacity, fading to half toward the bottom, with the desktop's noise tile as grain (overlay, up to 9%). Changes ease over 280 ms. The mode applies to the sidebar panel, the command bar, the page menu and the editor (`spaceAppearance`). Pages keep the system's. `theme.accent` (the color, or grey) tints the switcher icon and controls.
- **Editor.** The space menu's "Edit Theme" opens `ThemeEditor`, a sheet laid out after the desktop's:
  - mode buttons;
  - `ThemePad`: a hue wheel with the colors as dots. Hue is the angle (red at the top, clockwise) and saturation the distance. A drag keeps the dot's lightness, the big dot takes the others along, and a touch away from the dots moves the big one there;
  - − and + (1 to 3 colors, the new one 40° on);
  - the palette, which replaces the gradient, and "Any Color" (`ColorPicker`), which keeps it;
  - the nine gradient presets;
  - Intensity and Grain sliders, hidden for grey.

  The draft shows at once and reaches the store (and sync) at most every 50 ms.
- **Tests.** `SpaceThemeSyncTests` (encoding, stale rgb, round trips, equivalent encodings, saved files, remote apply without a commit back); UI test `ThemeEditorTests` (preset, − and +, dragging the big dot, mode, sliders, grey).

### Status bar over pages

Pages run under the status bar, but fixed and sticky elements (YouTube's header) would go behind it: WKWebView's content inset moves the page, not its layout viewport. `WebView` sets `obscuredContentInsets.top` (iOS 26) to the status bar's height (`BrowserScreen.statusInset`, measured, so it follows rotation), so WebKit lays them out below it, as in Safari. The same at the bottom: `obscuredContentInsets.bottom` is the bar's measured inset (`BrowserScreen.barInset`: the bar or its collapsed pill, the save banner when it shows, and the home indicator), so fixed footers (YouTube's tab bar) sit above the bar; it follows the bar as it collapses. Split panes sit under their title strip or the pane picker and get 0 at the top.

### Auto picture-in-picture

As on the desktop (picture-in-picture.md): leaving a tab whose video plays with sound shows the video in picture-in-picture, and coming back to the tab puts it back in the page. Scrolling a playing video off the screen does too (below). Leaving the app does too (below). Settings → Auto Picture in Picture turns it off.

- `AutoPiP` (App/Browser). When the shown tab changes (`AppModel.shownTabChanged()`, from `selectedNodeID` and `selectedPane`), a script in the tab left looks for the desktop's conditions: playing, not muted, volume above 0, at least 100×100, the only such video in the main frame, and no video already in picture-in-picture (the user's own stays). It calls `webkitSetPresentationMode("picture-in-picture")` (or `requestPictureInPicture()`), as an app-evaluated script counts as a user gesture. The tab shown takes its own video back. The script runs in its own content world, so the page doesn't see which video Compass moved.
- **Inline, not fullscreen** (`AutoPiP.inlineScript`, every frame, from the document's start, in the same world): on the iPhone WebKit plays a video without `playsinline` fullscreen as it starts, which YouTube's player did. Each video gets `playsInline` (and the attributes) as it's added, through a MutationObserver, before it can play. The player's own fullscreen button still goes fullscreen.
- **YouTube** sets `disablePictureInPicture` on its player, and pauses when the page goes hidden (background play is a Premium feature there), which ended the window at once. `disablePictureInPicture` doesn't make a video ineligible (the enter script clears it, as for calls), and while Compass's window shows the page's video (or is starting, 3 s), capture listeners on the window in the script's world stop the page's `visibilitychange`, `pagehide` and window `blur` events before the page's own listeners on the document see them. A site that polls `document.hidden` instead still pauses. WebKit also keeps `document.pictureInPictureElement` set on YouTube with no window open, so "a video already in picture-in-picture" is a video whose `webkitPresentationMode` is `picture-in-picture`, never that property (seen in Copy Diagnostics: every attempt said "already in PiP").
- The page reports (`state:playing` / `state:idle`) whether it has such a video, so on a tab switch Compass first **parks** the left tab's web view (a 1×1, nearly transparent view at the back of the window) and then starts picture-in-picture: a web view out of the window ends its window at once, and SwiftUI drops it on the same update. The view leaves the lot when the window closes or the tab is shown again (SwiftUI takes it back).
- The app needs the `.playback` audio session category (set at launch; WebKit leaves it alone) for picture-in-picture. As in Safari, page sound then ignores the silent switch.
- **Leaving the app**: WebKit in an app never starts picture-in-picture by itself for a video playing in the page. AVKit's "start automatically" is only armed for a video in fullscreen, or for the one standing by under an element in fullscreen (`WebFullScreenManager::setPIPStandbyElement`); `HTMLVideoElement.autoPictureInPicture` is ignored (the script sets it anyway). So `AutoPiP.watchLeavingTheApp` starts it on `UIApplication.willResignActiveNotification` (the home swipe's first move, while the app is still in front; SwiftUI's `scenePhase` comes later), for the shown tab if its page reports `state:playing` (`AppModel.leavingAppVideo()`). The earlier attempt (on `scenePhase` going inactive) saw the window open and close within a second, the video paused, most likely because the window takes a few hundred milliseconds to come up, and if the app reaches the background first WebKit counts the web view as unseen (`PageClientImpl::isActiveViewVisible`: only a window already up keeps it seen), so the page is hidden and frozen. Now the app holds a background task while it starts, and once the window is up (`requestPictureInPicture()` resolved: the page posts `log:in window`) with the app already in the background, the web view is taken out and put back in its place, the one thing that has WebKit look again (`didMoveToWindow`), and a page whose video is in picture-in-picture counts as seen. Back in the app the video returns to the page. Control Center and Notification Center also make the app resign active: the window opens and goes back when the app is active again.
- The window's "back to the page" button puts the video back inline, still playing: a user script reports that (`compassPiP`), and Compass shows the tab. Its close button pauses the video, so nothing happens.
- `UIBackgroundModes` has `audio`, so the video keeps playing outside the app.
- **Scrolled away** (Settings → When Scrolled Away, on by default, under the main switch): the script watches each video from its first play with an `IntersectionObserver`. A video meeting the conditions above that is less than half visible posts `scroll:out` (early, so the window grows from the video rather than a sliver at the screen's edge; visible share = visible height over the most that fits, for videos taller than the screen); Compass, if that tab is shown and the app is active, runs the enter script marked as a scroll (`__compassAutoPiPScroll`). Once 90% of that video is back on screen the page posts `scroll:in` (so it lands where it's seen; the gap keeps it from flipping). Either waits for the page to stand still (finger up, momentum over; `AppModel.settlePictureInPicture`), then Compass asks the page again (`__compassAutoPiPWant()`), because a window started mid-scroll grew from a frame the page had moved past, and jumped. Closing the window, or its "back to the page" button, while the video is off screen leaves it alone until it has been on screen again; "back to the page" also scrolls it into view (no tab to show: it's the shown one).
- **Calls** (Teams, Zoom; picture-in-picture.md, "Calls"): the desktop's `common/picture_in_picture/call_pip.js` is bundled and runs in the same content world (`AutoPiP.callScript`). While a call is on, the page reports `state:playing` (checked every second: a call starts with no media event of its own), and the enter script takes the call's pick (the largest remote tile; tiles are muted) instead of the single video with sound. The window doesn't follow the speaker: WebKit wants a gesture for each request. Leaving the app starts it as above, and iOS pauses the user's own camera while the app isn't in front. Meet's mobile site sends people to its app.
- Tests: `AutoPiPTests` (a local page and a real tap on play: the video is found and is back inline and playing after the switch). The simulator has no picture-in-picture at all (even Safari reports `pictureInPictureEnabled` false), so the window itself needs a device.

### Undo closing

The desktop's Cmd+Z / Cmd+Shift+Z for closed entries (sidebar.md, "Undo closing"), on the phone.

- **The stack.** `ClosedEntries` (CompassModel), one on `AppServices` for every window, in memory, the last 25 steps. A step is what one user action closed. Each entry keeps its `SidebarNode` itself and its place: the parent, the sibling before it and its index, and for a pane whose split of two ended, the other pane. A removed node stays detached with its subtree, so it comes back under its IDs, custom title and pinned URL included; sync creates it again over its tombstone (sync.md, "Create over a tombstone"). The step also keeps the `WKWebView.interactionState` of each tab that was open: the tab that opens again on undo gets its back and forward history back (`TabManager.restoring`), as TabRestoreService gives the desktop.
- **Recording** (`AppModel.userClose`), user closes only: ⌘W, a flicked card in the tab picker (both: Today to the archive, anything else unloads, `.archive` / `.unload`), a Today row's ×, Delete and Remove from Favorites (`.remove`), and Today's Clear (every entry, one step). Not the menu's Archive, auto-archive, sync, Claude's or an extension's closes, or Close All Incognito (which drops every incognito step: `dropIncognito()`).
- **Undo** (`undoClose()`): the entries come back, last closed first, after the sibling they followed (a parent that's gone sends them to the top of Today, the end of Pinned, or the favorites); an archived entry moves back out of the archive, a pinned tab or favorite goes back to the page it showed. The tab shown as it closed (else the first that was open) opens and is shown; the others come back unloaded. With none open, the window goes to the step's space. Nothing crosses in or out of incognito (`SpaceStore.reinsert`).
- **Redo** (`redoClose()`) closes the step's entries again, the same way, keeping the other redos. Any other close drops them.
- **UI.**
  - A toast over the bar after each close, "Closed “<title>”" (or "Closed 3 tabs") with **Undo**; after an undo, "Reopened …" with **Redo**. Four seconds, replaced by the next one; a flick down puts it away.
  - **Shake to undo**: a shake shows iOS's alert for it, "Undo Close Tab" with the entry, Undo, Redo when there is one, and Cancel ("Redo Close Tab" once only redo is left). While the keyboard is up the shake is the text's own Undo Typing (`UIWindow.motionEnded`, `KeyboardWatch`).
  - **⌘⇧T** (Tabs menu, "Reopen Closed Tab") undoes with a hardware keyboard. ⌘Z and ⌘⇧Z stay the text's: iOS gives a page no way to pass them on, as the desktop's undo waits for.
- Tests: `ClosedEntriesTests` (places, Clear Today's order, a folder's tree, a pane back in its split, a gone parent, a pinned tab's page, incognito, steps and redo, the cap).

### Peek

The desktop's Peek ([peek.md](peek.md)) for touch: look at a link without
opening a tab.

- **Long-press a link.** The preview is the page itself, loading in a web
  view of the tab's profile (its cookies, scripts, extensions, filter
  lists). Tapping the preview, or **Peek** at the top of the menu, shows
  that same page in Peek's card, no reload. The system's items (Open Link,
  Copy Link, Share…) and the extensions' items stay.
- **The long-press menu** on a link, after Peek: Open in New Tab, Open in
  Background (a Today entry, not shown), Open in Incognito, Open in Little
  Window and Pin (a new Pinned entry of the tab's space). They use the
  tab's space, so the link keeps its profile. In an incognito tab only the
  first two show: its links stay incognito. Then the system's items (Open
  Link, Copy Link, Share…) and the extensions'.
- **Links from pinned tabs and favorites** that open a new tab or window
  (`target=_blank`, `window.open()` on a tap) peek too, so a pinned app
  stays put, as on the desktop. The page is made from WebKit's own
  configuration, so it keeps `window.opener`.
- **The card** is a sheet: on an iPhone the large detent (swipe down to
  close), on an iPad page-sized over the dimmed window (iOS 18; a form
  sheet before). A bar above the page shows its title (or only its host
  until it has one), **Open** and close.
- **Close:** the close button, Esc, ⌘W, swiping it down (tapping outside
  on an iPad), `window.close()`, or another tab shown. The page goes.
- **Open** (⌘O): the page becomes the newest Today entry of its space,
  with its history, scroll position and form state
  (`TabManager.adopt`). A favorite's page goes to the window's space if it
  has the same profile, else that profile's first space.
- Inside the card, links load in the card; a link that opens a new tab
  opens a Today entry, which closes the card. A long press in the card is
  WebKit's own menu (its preview opens the link in the card).

How: `TabManager.makeWebView(for:url:configuration:)` builds a web view as
a tab's would be, without making it a tab; `webView(for:)` uses it for
tabs, and `adopt(_:as:)` makes one a live tab later. The long-press
preview's page waits in `TabManager`'s preview slot until the menu commits
(`contextMenuForElement:willCommitWithAnimator:`) or ends. `onPeek` hands
pages to the window of the tab they came from (`AppServices.owner(of:)`),
whose `AppModel.peek` the card shows (`App/Browser/Peek.swift`).

Not done: the page in the card has no password AutoFill, isn't in history
until opened, and Esc goes to the card before the page.

### Little windows

The desktop's little windows ([little-window.md](little-window.md)) on an
iPhone: with Compass as the default browser, a web link from another app
(Mail, Messages, Slack) doesn't land among the tabs. It opens in Peek's
card, over whatever the window shows, in its little-window mode:

- The page is of the shown space's profile (the first space's, when
  incognito is shown: a link from outside never opens incognito).
- The bar has back (no forward, as on the desktop), the address,
  **Open in <space>** naming the shown space, and a **⌄** listing the
  profile's spaces when it has more than one (same profile only: the page
  can't change its website data). Either makes the page the newest Today
  entry of that space, as it is, and shows it.
- Closing it (the close button, Esc, ⌘W, a swipe down) lets the page go:
  it never reached the sidebar. iOS's own "◀ Mail" in the status bar goes
  back to the app the link came from.
- The address shows the title and host; tapping it (or ⌘L) edits the URL,
  and Go loads what's typed (an address, or else a search) in the card.
- A second link replaces the first. With a keyboard, ⌘[ and ⌘] go back and
  forward in the card's page, ⌘R reloads it and ⌘O opens it.
- Settings → Links from Other Apps:
  - **Open in a Little Window** (on): off, links from other apps open as
    new Today entries of the shown space, as before.
  - **Share Sheet Opens a Little Window** (off): on, "Compass Browser" in
    the share sheet skips its form (Open or Add to Pinned, the space, the
    folder) and opens the link in a little window: an Inbox request with
    the `little` action. It's in the App Group's defaults
    (`SharedContainer.shareOpensLittleWindowKey`), where the extension
    reads it.

Links from Compass's own extensions and widgets (`compass://open`) still
open as tabs: they name their space.

**iPad.** When the window the link arrives in is one of several on screen
(smaller than the screen: Stage Manager, iPadOS's windowed apps, Split
View), the little window is a window of its own, centered over the others:
the same bar and page, 720×900 by default. Open in <space> moves the page
into the last active browser window (or a new one, with none open) and
brings it to the front; close, Esc or ⌘W closes the window. Keyboard
commands (⌘L, ⌘O, ⌘[, ⌘R…) act on the little window while it's the key
one. A full-screen window gets the card, as on an iPhone.

How: the page is made as for the card and waits in `AppServices`
(`openLittleWindow`) while a scene session is requested
(`activateSceneSession`, `UIWindowSceneProminentPlacement`) with an
`NSUserActivity` of type and `targetContentIdentifier`
`it.cipi1965.compass.little-window`. A second `WindowGroup(id: "little")`
matches it (`handlesExternalEvents(matching:)`); the browser windows don't
claim it, having no `handlesExternalEvents(preferring:allowing:)`, so
SwiftUI makes a new little scene, whose `LittleWindowScene` takes the
waiting page. One iPadOS restores after a relaunch has no page and closes.
If iPadOS refuses the request, the card shows instead.

Not done: the address has no suggestions.

### Incognito

An incognito space at the end of the space switcher; its tabs kept in memory while the app runs, locked behind Face ID (or the passcode) once the app has been in the background, gone when it quits; saved logins still fill; extensions only with a per-extension "Allow in Incognito" (off by default, as in Chrome); no Claude (as the desktop).

- **Model.** `SpaceStore.incognito`, a `Space` with `isIncognito`, kept out of `spaces`, so nothing that goes through them sees it: saving, sync's bridges, sessions, widgets, Spotlight, Claude's tab list. Lookups (`node(id:)`, `space(id:)`, `space(of:)`) include it. Its local changes are dropped before `onLocalChange` (every ID ever seen under it, so deletions too). `canMove` refuses moves across the boundary. Archiving an incognito entry removes it. Tests: `IncognitoTests`.
- **Pages.** One `WKWebsiteDataStore.nonPersistent()` for every incognito tab, made new by "Close All Incognito Tabs" (`TabManager.resetIncognito`).
- **No traces:**
  - no history visits;
  - favicons in memory only: hosts an incognito tab navigates to, or any incognito entry is on, are never written to Caches/Favicons, offered to sync or copied for the widgets;
  - no password save offers (fill still works);
  - the last shown tab and space aren't remembered;
  - iPadOS state restoration doesn't get the entry.
- **Lock.**
  - Inactive covers incognito (`incognitoCovered`), so the app switcher's snapshot shows nothing.
  - The background locks it (`incognitoLocked`) if it has tabs and the device can authenticate.
  - `IncognitoLockView` covers an incognito page (bar included) and the incognito sidebar page. It asks for Face ID when the app comes back, and has Unlock and "Show Other Tabs".
  - The tab picker leaves locked incognito tabs out.
- **UI.**
  - The incognito page: dark (`IncognitoLook`), Today only, a note on what incognito does, and a menu with "Close All Incognito Tabs".
  - The switcher shows glasses for it.
  - The bar is dark on incognito pages; pages keep their own look.
  - An incognito entry's menu has no Pin, Archive, Add to Favorites or Move to Space; Pin is disabled in the page menu.
  - New Incognito Tab: ⌘⇧N, a long press on either "+" (the sidebar's, the bar's), and the Home Screen icon's menu (`compass://incognito`).
- **Extensions.** The incognito space is a private window (`ExtensionWindow.isPrivate`); a context gets `hasAccessToPrivateData` from the extension's "Allow in Incognito" switch.
- **Claude.** No Ask Claude tile or ⌘⇧E on incognito tabs; the sheet says "Not in Incognito"; tools refuse incognito tabs.
- Debug: `-CompassIncognitoURL <url>`.

### iPad

- The app (and its extensions) run on iPad (`TARGETED_DEVICE_FAMILY` 1,2; every orientation, for Split View and Stage Manager).
- A wide window (iPad, regular width; `AppModel.isWide`, set by `RootView`; not a Max iPhone in landscape) has the desktop's layout: the sidebar (`SidebarScreen`) is a column on the left, in the space's tint, beside the browser, instead of the bottom panel. The bar gets a sidebar button on its left that hides or shows the column (`showingSidebarColumn`); a swipe up on the bar's middle shows it. Narrow (Slide Over, a narrow Split View, a narrow Stage Manager window: compact width) is the phone's layout.
- **The column resizes** as the desktop's: its trailing edge (16 pt, over the page's side) drags it between 240 and 480 pt, a double tap puts it back to 320, and VoiceOver adjusts it in 20 pt steps (`RootView.resizeEdge`). The width is kept (`SidebarColumnWidth`) for every window and the next launch. The page keeps at least 400 pt beside it: in a smaller window the column gives way, down to 240.
- A split shows its panes side by side, each under a title strip; the front pane (`selectedPane`: the bar, address, back and forward act on it) is outlined in the space's accent, and a tap on a pane's strip brings it to the front. Side by side needs 280 pt a pane (`AppModel.minPaneWidth`, against the measured `pageWidth`): in a smaller Stage Manager window, or beside a wide column, a split of two is stacked and a bigger one shows a pane at a time under the segmented picker, as on the phone.
- **Dragging panes** (`PaneStrip`): a pane's strip drags as its entry. Dropped on another pane's strip, a pane of the same split moves to that pane's left or right (the half it's dropped on, mirrored right to left), the front pane staying in front; any other tab (a sidebar row, another split's pane) joins the split there and comes to the front, while the split has room (four panes), as the desktop's split rows take drops (sidebar.md). Favorites, folders and splits don't go in. The strip highlights while a drop is over it. Dragged to the sidebar, a pane drops as any row does. `AppModel.dropOnPane`.
- Bar swipes between entries move by the page's width (measured), not the window's. `SidebarPanel`'s sizes are the window's, not the screen's.
- Debug: `-CompassOpenURLs "<a> <b>" -CompassSplit YES` splits the last two tabs.
- **Keyboard shortcuts** (`BrowserCommands`, KeyboardShortcuts.swift; a hardware keyboard on iPad or iPhone; listed in the iPadOS menu bar and the ⌘-hold overlay): ⌘T new tab and ⌘L the address (command bar), ⌘S the sidebar, ⌘R and ⌘⇧R reload, ⌘[ and ⌘] back and forward, ⌘⇧C copy link, ⌘F find, ⌃Tab and ⌃⇧Tab the next and previous entry (wrapping), ⌘W close the tab (Today to the archive, others unload; Peek's card first), ⌃1–9 the spaces, ⌘N a new window, ⌘⇧N a new incognito tab, ⌘⇧T reopen a closed tab. Debug: `-CompassFreshStore YES` starts from an empty sidebar.
- **⌘W, ⌃Tab and ⌃⇧Tab come first** (`BrowserKeys`). As SwiftUI commands they didn't reliably reach the app: the system's Close Window kept ⌘W (Close Tab in `.saveItem` didn't help, and an AppDelegate `buildMenu` override isn't called under SwiftUI's lifecycle), a focused page took Tab (⌃Tab worked once), and root view controller key commands with `wantsPriorityOverSystemBehavior` didn't help either. So `UIApplication.sendEvent(_:)` is swapped for `compassSendEvent`, which looks at hardware key presses before anything else in the app does (a focused page's included: logged, ⌃Tab's presses arrive there and stop): in a browser window (one with an `AppModel`; a little window keeps its own ⌘W) those keys run their command and the press and its release go no further. With a sheet up (Settings, the command bar, an extension's popup) ⌃Tab passes through and ⌘W does nothing, except on Peek's card, which it closes. The Tabs menu still lists them (picked from the menu bar they act the same; Close Tab in a little window closes it).
- **⌘W in the iPadOS 27 simulator** never reaches the app: not even the ⌘ press of the chord gets to `sendEvent` (⌘T's does), and nothing happens; once, right after the simulator booted, it did arrive and closed the tab. Removing the system's Close Window from the main menu (a swapped `buildMenu`) and giving the menu's Close Tab `wantsPriorityOverSystemBehavior` changed nothing, so neither is kept. It looks like the system's window management keeping ⌘W; on an iPad it's still to be checked (Open items). The pre-existing ⌘T test fails the same way in the simulator (its presses arrive, but the command bar doesn't open), also on main before these changes.
- **Extensions' shortcuts.** Web extensions' manifest `commands` with keys (`WKWebExtension.Command`'s `activationKey` and `modifierFlags`, as WebKit reads them) run through `BrowserKeys` too: the press goes to the extension (`performCommand`: `commands.onCommand`, or its action for `_execute_action`) on the shown tab. Keys the browser or editing keep (above, ⌘C/V/X/A/Z/Q/H) stay the browser's, as Chrome keeps its own; keys with no modifier are never taken. The menu bar's Extensions menu lists every running extension's commands ("<extension>: <description>") with their keys.
- Tests: `KeyboardShortcutsTests` (iPad simulator: ⌘T, ⌘S; ⌃Tab twice and ⌃⇧Tab with the page focused; the column's edge drags wider and a double tap restores it) and `SplitPaneDragTests` (a pane's strip dragged onto the other's right half swaps them; drag and drop in the simulator is unreliable, it passed 2 of 3 runs).

### Multiple windows

`UIApplicationSupportsMultipleScenes`; one `WindowGroup(for: WindowTarget.self)`, whose value is the entry the window shows (so iPadOS restores each window on it) plus a token (SwiftUI would bring up an open window with an equal value instead of opening one).

- **Split of the model.** `AppServices` (one per app): the store, live tabs, sync, passwords, extensions, downloads, system bridges, saving, the archive timer, and every service callback. `AppModel` (one per window, made by `WindowRoot`): the space and entry shown, sidebar, sheets, tab picker, bar; it forwards `store`, `tabs`, `sync`… so views are unchanged. Tab callbacks (scroll, navigation, picture-in-picture) go to the window showing the tab; window-less ones (a page's `window.open`, extensions, widgets' links, shortcuts, sent tabs, quick actions) to the **active window**, the key one (`KeyWindowReader`, `UIWindow.didBecomeKeyNotification`; scene phase can't tell: side-by-side windows are both active). Links and notification taps that come before the first window wait for it.
- **One page, one window** (the desktop's move on focus, sidebar.md): a WKWebView is in one window at a time. `AppServices.owner(of:)`: the window that last took the tab (`claim`) while it still shows it, else the active window showing it. A window takes the tabs it shows when it becomes key, and a tab picked in it. Another window showing the same entry shows `ElsewherePage`: the page's snapshot, "Open in another window" and **Bring Here**. `WebView` puts the page in a container of its own, so SwiftUI removing one window's view never pulls the page out of the other.
- **Opening one:** ⌘N (New Window, on the space the active window shows), "Open in New Window" in an entry's long-press menu (the page moves there), dragging a tab out, and the system's own (the app icon's menu, the dock, dragging to the side).
- **Dragging a tab out** (`TabDrag`): a sidebar row or a split pane's title strip drags with the entry's ID as text (the sidebar's drops) and, on iPad, an `NSUserActivity` (`it.cipi1965.compass.tab`, in `NSUserActivityTypes`) holding the entry. Dropped where no window takes it (beside the window, on the screen's edge), iPadOS opens a scene for the activity. It has no target content identifier, so the browser windows' group gets it, not the little windows'. `AppDelegate` notes the entry by the new scene's session (`AppServices.noteDraggedOut`), and the window, once it knows its scene, shows the entry and takes its page, as Open in New Window; `onContinueUserActivity` does the same if SwiftUI hands the activity to a window instead. A pane opens its split, with that pane in front. Folders drag only within the sidebar.
- Debug: `-CompassSecondWindow YES` opens the first window's entry in a second window 8 s in. The simulator can't show two windows side by side headless, so placeholders, Bring Here and move on focus need an iPad (Stage Manager or Split View).

## Web extensions

WebKit's `WKWebExtension` API (iOS 18.4+) runs Chrome and Firefox (WebExtension) extensions in a WKWebView app. Compass still targets iOS 17, so it's all behind `#available(iOS 18.4, *)` and hidden on older systems.

- **Install.** From a Chrome Web Store or Firefox Add-ons link or ID (Settings → Extensions), from "Add to Compass" in the page menu on a store's extension page, or from a .crx/.xpi/.zip in Files. `ExtensionSource` downloads the package: a CRX from Chrome's update service (`clients2.google.com/service/update2/crx`, `prodversion` 142), or the XPI the AMO API names. The CRX2/3 header is stripped. An install sheet (`ExtensionInstallSheet`) shows the icon, the version and what the extension can do (`ExtensionPermissions.summary`) before anything runs. Adding a store extension that's already there updates it in place, keeping its data.
- **Storage.** `Application Support/Compass/Extensions/<id>/`, unpacked (`ExtensionPackage`: a central-directory ZIP reader on Compression's raw deflate, refusing paths outside the folder), plus `Extensions.json` (source, on/off, optional grants). A Chrome extension keeps its Chrome ID as the context's unique identifier (its storage is keyed by it). Other sources get a UUID.
- **Same origin every launch.** WebKit gives a context a random base URL unless the app sets one, and an extension's IndexedDB and localStorage belong to that origin, so they'd be lost at every restart (1Password keeps its accounts in IndexedDB). `context.baseURL` is `webkit-extension://<record id>/`.
- **Service workers.** WKWebView runs service workers only in apps with the default-browser entitlement or App-Bound Domains, so a background `service_worker` never starts in Compass (context error 6 after 30 s). At install, `ExtensionPackage.adaptManifest` rewrites it as `background.scripts` (non-persistent background page; `type: module` kept). For classic workers, scripts named literally in `importScripts()` are listed first and `importScripts` becomes a no-op. Firefox extensions already use `background.scripts`. Revisit if Apple grants the browser entitlement.
- **Running.** `WebExtensions` (Extensions/WebExtensions.swift) owns one `WKWebExtensionController` (`.default()`, persistent). Every tab's configuration gets `webExtensionController` (`TabManager.webView(for:)`), whatever its profile's data store, so extensions reach every profile. Their background pages use the default store. Requested permissions and match patterns (not the optional ones) are granted at load: the user agreed in the install sheet. Optional ones are asked for with an alert, and the answer is kept in `Extensions.json`. The background starts at load (`loadBackgroundContent`), and live tabs the extension injects into are reloaded, since pages that loaded before it started have no content scripts.
- **Tabs and windows.** Each space is a window (`ExtensionWindow`), and its pinned and Today tabs, plus favorites in the shown space, are its tabs (`ExtensionTab`, keyed by entry ID; an entry without a web view is an unloaded tab). The shown space is the focused window. AppModel and TabManager report opens, closes, activation, focus and URL/title/loading changes. Every web view is announced as it's created (`TabManager.webView(for:)`), once per tab, and when an extension loads, the tabs already live are announced and the shown one activated; otherwise a tab restored after a relaunch is unknown to extensions (1Password then finds no tab for the page's messages). Extensions can open tabs (Today in their window's space, or pinned), load, reload, go back/forward, pin, close (Today goes to the archive), duplicate and snapshot. `windows.create` opens its tabs in the shown space.
- **Extension pages in tabs.** A tab showing an extension page has the extension's web view configuration, which won't load websites, so when such a page navigates to a website (1Password's "Sign in" sets `location.href`), `TabManager` cancels that navigation and starts the tab over as a web page at the address.
- **UI.** The page menu shows a row of action buttons (icon and badge). A tap closes the menu and runs the action: a popup comes up as a sheet (`popupViewController`, medium/large). Settings → Extensions lists them (on/off, swipe to remove), and the detail screen has what it can do, problems WebKit reported, Options (in a sheet, the extension's own web view configuration), Update, Show in store and Remove (data included).
- **Popup title.** WebKit draws the popup's title over the page in the system's style, which is unreadable on a dark popup, and changing that controller's interface style hangs it. So the title is empty, and the popup names itself.
- **Missing APIs (polyfill).** WebKit on iOS lacks namespaces desktop extensions use (`notifications`, `idle`, `management`, `privacy`, `downloads`, …) and some members (`webNavigation.onCreatedNavigationTarget`, `storage.managed`, `tabs.onReplaced`, …). Touching one throws, and the background never starts (1Password: `chrome.notifications.onClicked` on its first line). `compass-polyfill.js` runs first in every HTML page of an extension (an entry module for module backgrounds), and adds stubs: events that never fire, and calls that resolve to `[]` for queries, `{}` for getters and `"active"` for `idle.queryState`. It also adds the service-worker globals a background page lacks (`clients`, `registration`, `skipWaiting`, `importScripts`). Additions are non-configurable, because WebKit redefines its namespaces' properties after startup and drops configurable ones. WebKit also swaps some namespace objects after startup (`storage`, losing `managed`), so after patching the polyfill pins each namespace on `chrome` (non-configurable). The globals stay WebKit's own: replacing `chrome`/`browser` with a proxy stops events reaching listeners (`action.onClicked` never fired). `action.openPopup()`, which iOS WebKit lacks, opens the popup through native messaging (`compass-open-popup`), when the action has one.
- **Background lifetime.** WebKit unloads an idle background page after about 5 s. Its IndexedDB connections close first, with a `close` event Chrome never sends when it stops a service worker, and 1Password takes that for a crash. The polyfill drops IDBDatabase `close` events, and a keep-alive page holds the background loaded: `compass-keep-alive.html` in a 1 pt, nearly transparent web view in the app's window (`WebExtensions.keepAlive`), with a `runtime.connect` port to the background, pinged every 4 s and held by the polyfill.
- **Generated files.** `ExtensionPackage.refresh` rewrites the polyfill, debug and keep-alive files before every load (and adapts folders from before the polyfill), so an extension installed by an older build gets the current ones.
- **`tabs.query` and URL access.** WebKit ends every `tabs.query` by asking the delegate for permission to the matched tabs' URLs. Compass answers right away with what's already granted, without a prompt (Chrome never asks there), so tabs the extension can't reach have no URL or title. A prompt that never completed would make every `tabs.query` hang. The remaining prompts (`permissions.request`) always answer, and say no if their alert can't be shown.
- **Ports from extension frames.** WebKit disconnects a `runtime.connect` port opened from an extension frame inside a web page a few ms after the background's onConnect, when the frame connects some time after loading (300 ms or 1 s in: lost; at once: kept), with no `lastError` and no `disconnect()` on either side. `runtime.sendMessage` from the same frame always gets through, and so do content scripts' ports. 1Password's "Save passkey?" sheet (`inline/notification/notification.html`) connects about 120 ms in and lost its port. So the polyfill emulates ports for extension frames inside tabs (`ExtensionPackage.framePorts`). There, `runtime.connect()` returns a Port of ours: its messages go out as `sendMessage` calls, in order, and the background's come back as replies to a long poll (held 10 s, then polled again). In the background (`/_generated_background_page.html` or the manifest's page), onConnect listeners get a matching Port with the real `sender` (tab, frame, URL). Both sides' `disconnect()` reach the other; a frame that goes away sends a close on pagehide, and one that goes silent (no poll for 25 s) is disconnected. Every context's onMessage listeners skip this traffic (key `__compassPort`).
- **Extension frames at phone width.** 1Password's sheet is a full-screen iframe with the card at `width: 400px`, 16 px from the right, so on a 402 pt iPhone it ran past the left edge. The polyfill, in an extension page shown in a frame narrower than 480 px, gives any box at least half the frame wide that runs past an edge `max-width: calc(100vw - 32px)` (once per box; measuring its margin mid-animation squeezed it to half).
- **Native messaging.** There's no native app to talk to, so `sendNativeMessage` and `connectNative` fail at once, and 1Password falls back to working alone. Debug builds add `compass-debug.js` (and the `nativeMessaging` permission), which forwards console errors and warnings, uncaught errors and rejections to the app log (`WebExtensions: [name] …`). `-CompassNoPolyfill YES` installs without the polyfill.
- **Passkeys through 1Password.** They need no change in Compass and no Apple entitlement. 1Password doesn't use the browser's WebAuthn: a content script in the page's own world (`inline/injected/webauthn-listeners.js`, `"world": "MAIN"`, document_start) replaces `navigator.credentials.create`/`get` and `PublicKeyCredential`'s capability checks with locked getters, and its background makes the passkey. WKWebExtension runs MAIN-world content scripts, and Compass's web views have `navigator.credentials` and `PublicKeyCredential` (secure context). The system passkey sheet (iCloud Keychain, the 1Password app) would need `com.apple.developer.web-browser.public-key-credential`, which Apple grants only to default browsers.
- **Face ID unlock** (`ExtensionUnlock`). Desktop 1Password unlocks with the desktop app's biometrics over native messaging; on iPhone nothing can offer that. A script in the extension's own pages (popup, options, its pages in tabs) watches for a visible password field. After the user types the password and the field goes away, Compass offers "Unlock <name> with Face ID?" and keeps the password in the Keychain, on this device only (`WhenPasscodeSetThisDeviceOnly` + `biometryCurrentSet`). After that, a password field shows up → Face ID → the password is typed in (native value setter + input events, for React) and submitted as a user would: Enter in the field (1Password's unlock listens for it), then, if the field is still there after 0.5 s and isn't busy, a click on its submit button, then the form. The button is the one holding an `unlock-icon` (1Password's arrow, labelled "Unlock"), else a submit button, else one labelled like unlock, sign in or continue (several languages), else the last one after the field. Show/hide toggles (labels with password/reveal/show/hide/mostra/nascondi…, or `aria-pressed`) are never clicked: 1Password's "Reveal password" is the first button after its field. (`form.requestSubmit()` did nothing in 1Password.) If the field is still there 10 s later, the password is forgotten and an alert says so. "Not Now" stops the offers for that extension. The detail screen's "Unlock with Face ID" switch forgets it, and so does removing the extension. Debug: `-CompassPopupJS <js>` runs in the popup, and `-CompassUnlockAutoSave YES` skips the offer. Simulator builds need `CODE_SIGN_IDENTITY=-` for the Keychain (unsigned: error -34018).
- **The desktop's extensions.** `ExtensionsBridge` (CompassSync) downloads EXTENSIONS (field 48119) in every profile's account. It's download only and mirrored to `sync-extensions.json`. Settings → Extensions → "On Your Other Devices" lists the enabled Chrome Web Store ones the phone doesn't have, each with Add (which goes through the install sheet). Tests: `ExtensionsBridgeTests`, plus `ExtensionsServerTests` (a Mac-side engine puts one, and the phone's bridge gets it from the real server).
- **Updates.** At launch, at most once a day (or with "Check for Updates"), each store extension's current version comes from Chrome's update service (`v=0.0.0.0`, the `<updatecheck version>`) or the AMO API. A newer version that asks for nothing more (`ExtensionPermissions.asksForMore`) is installed quietly, keeping its state, data and grants. One that asks for more shows "Update available" and "Review Update to x", which goes through the install sheet.
- **Website access.** "All Sites It Asks For" (the default) or "Only These Sites", a list of hosts (each with its subdomains): only those match patterns are granted, so content scripts run only there. A long press on an extension's button in the page menu shows its own menu items, "Allow on <site>" (when limited), Options and Turn Off (`MenuButton`, a UIKit button, since extension menu items are `UIMenuElement`s).
- **Context menus.** A long press on a page adds each extension's `menuItems(for: tab)` after the system's items, under the extension's name when it has several (`TabManager`'s `contextMenuConfigurationFor`).
- **App Review.** Guideline 2.5.2 bars downloading code that changes the app. Extensions only run in web content, as in Orion, which ships Chrome and Firefox extensions on the App Store, but review may ask.
- **Known to work:** a test MV3 extension with a service worker (content script, `runtime.sendMessage` reply with `sender.tab`, `tabs.create`, `storage`, `action.setBadgeText` showing on the menu button), Dark Reader, uBlock Origin Lite, and 1Password (installs from the Chrome Web Store, its WebAssembly core starts, its passkey hooks run). Debug launch args: `-CompassAddExtension <link, ID or /path.zip>` adds without the sheet (`-CompassExtensionSheet YES` stops at it), `-CompassExtensionAction <n>` taps the menu's nth extension after 8 s.

## System extensions

Everything goes through the App Group `group.it.cipi1965.compass`, which the app, Share, Widgets and SafariOpen are entitled to:

- **CompassShared** (CompassKit). `Snapshot.json` is the spaces as the extensions see them: style, glyph, favorites, Pinned tabs and folders, Today newest first, at most 16 entries a list, plus other devices' tabs. `Inbox/` holds one JSON file per request (open, or pin to a space or folder). `Focus.json` is the Focus filter's space. `Icons/<host>.png` are copies of the favicons. `DeepLink` covers `compass://open|node|space|search|new|inbox|downloads`.
- **App** (`App/System/`).
  - SystemBridge writes the snapshot a second after a change, then reloads the widgets, Spotlight and the icon menu.
  - It drains the Inbox at launch, when the app comes to the front, and on the Darwin notification `it.cipi1965.compass.shared-changed`.
- **Share** asks Open or Add to Pinned, then the space and the folder. Open then calls the host app's `openURL:` by selector to bring Compass up; if that doesn't work, the request waits for the next launch.
- **Widgets** (with `Shared/`):
  - Space (S/M/L), Today (M/L), Quick Actions (S plus Lock Screen circular, rectangular and inline), Other Devices (M/L);
  - the New Tab control (iOS 18);
  - the download Live Activity.
  - Small widgets use `OpenLinkIntent` buttons, since they don't support links.
- **Focus filter** (`CompassFocusFilter`): the space shown while that Focus is on, optionally the only one in the sidebar.
- **Spotlight**: spaces (item ID `space:<id>`; a tap shows its sidebar), and every favorite and Pinned entry, not just the widgets' first 16 (with the node ID as the item ID). Off with Settings → Show in Spotlight. One more item, `compass.app`, stays on regardless: the app itself with the keywords "browser", "web" and "internet". (The Home Screen name stays "Compass Browser": App Store Connect refuses "Compass" as a display name too, ITMS-90129.)
- **Icon menu**: New Tab, Search and the 3 most recent spaces.
- **SafariOpen**: a Safari web extension (popup) with Open, or Pin to a space. It writes to the Inbox, then sends the tab to `compass://inbox`.
- **Shortcuts** (app only, run in the app's process): Add to Space (Pinned or Today), Open URL in Space, Get Pinned Tabs, Get Today Tabs (tabs and folders, each with its folder path) and Get Spaces. There are no Siri phrases yet (`AppShortcutsProvider`).
- **Background downloads** (`Downloads+Background.swift`).
  - A page's GET download over http(s) leaves WebKit for the background `URLSession` `it.cipi1965.compass.downloads`, with the tab's cookies (filtered by domain, path and secure), its user agent and the referrer. POST, `blob:` and `data:` downloads stay with WebKit and pause when the app goes to the background (the Live Activity then says "paused").
  - iOS wakes the app when a download ends (`handleEventsForBackgroundURLSession`). Tasks survive a relaunch and come back to the list (`taskDescription` = item ID + name).
  - The Live Activity says "Downloading in the background" while the app is away.
  - Cost: the server sees the request twice (WebKit's, cancelled once it turns into a download, then the session's), so a one-time download link could fail.

## Tabs sent from the desktop

Design and the desktop and server parts: [send-tab.md](send-tab.md).

- `DeviceInfoBridge` sets `feature_fields.send_tab_to_self_receiving_enabled` (and the push receiving type), and refreshes an entry from before that.
- `SendTabBridge` (CompassSync) syncs SEND_TAB_TO_SELF: entries for this phone's cache GUID, unopened, at most 10 days old, http(s), each reported once (the GUIDs shown are kept in `sync-sent-tabs.json`, per account). `markOpened` sets `opened` and `notification_dismissed`.
- `SentTabNotifications` (App/System) posts "Tab from <device>", the title and host, also with Compass open (a banner). A tap opens the tab in a space of the account it came in and marks it opened. Notification permission is asked when the app comes to the front with sync on.
- Without a push, a tab arrives at once while Compass is open (the SSE stream), else at the next background refresh or launch.
- **Push** (`Push`, CompassSync): once notifications are allowed the app registers for remote notifications (every launch) and POSTs the device token to `/v2/compass/push` for each account (sandbox in Debug, production otherwise); leaving the chain DELETEs it. The app writes `PushAccounts.json` (profile, cache GUID per account) to the App Group.
- **PushService** (notification service extension, `it.cipi1965.compass.push`): reads the seed and server from the shared Keychain group and the accounts from `PushAccounts.json`, runs `Push.sentTabs` (a throwaway engine with only SEND_TAB_TO_SELF, so the app's sync state isn't touched) per account, and rewrites the server's "New tab" into the newest tab not delivered yet; none for this phone → "A tab was sent to another of your devices". The app skips its own notification for a GUID already delivered.
- Simulator limits: ad-hoc signed builds get no APNs token, and `simctl push` doesn't run service extensions.
- Tests: `PushServerTests` (register, a bad token refused, unregister, and the extension's fetch finds only this phone's tab against the real server), `SendTabBridgeTests`, `SendTabServerTests` (a Mac engine sends, the phone gets it once, opening marks it for the Mac). `SeedForSimulator` with `COMPASS_SEED_SEND_TAB=<url>` sends to every other device.

## TestFlight

- Team `4M33B5HNDS`, automatic signing. The entitlements are in `project.yml`: the Keychain group `<team>.it.cipi1965.compass.shared` in every target that needs it (no capability needed on the developer site), and the AutoFill Credential Provider in both the app and the extension (a capability of the App IDs; App Store Connect refuses the upload if the app lacks it).
- The bundle ID is `it.cipi1965.compass`. "Compass" is taken as a bundle name on the App Store (ITMS-90129), so the app's name is "Compass Browser".
- `ITSAppUsesNonExemptEncryption = false` (standard algorithms protecting the user's own data) and `App/PrivacyInfo.xcprivacy` (no tracking, no collected data, `UserDefaults` CA92.1).
- `compass ios archive` makes a Release archive with the build number set to the time (`YYYYMMDD.HHMM`) and exports `ios/build/archive/export/Compass.ipa`. `--upload` sends it to App Store Connect instead; `--api-key/--api-key-id/--api-issuer` sign with an API key when Xcode has no account.

## Verification

- **Unit tests:** `compass ios test` (= `swift test` in `ios/CompassKit`). It covers the crypto vectors, the UniquePosition port, the model invariants (ported from the SpaceModel unit tests) and the apply rules: unknown parent, cycle, invalid placement, delete subtree. Run only the suites a change touches.
- **Engine against the server:** server test suites start the repo's Go server on a random port and check two Swift clients on one chain, for each type (above).
- **Simulator:** run headless (`xcrun simctl`, with screenshots through `simctl io screenshot`), never in front of the user. A debug build launched with `-CompassServer <url> -CompassJoinRecovery "<words>"` joins a chain seeded by `SeedForSimulator` (a test that plays "the Mac"; `COMPASS_SEED_PUSH=1` keeps writing, `COMPASS_SEED_PROFILES=1` makes two profiles). `-CompassShow <title>` opens an entry, `-CompassSpace <name>` picks a space page and `-CompassJoinSummary YES` shows the join summary.
- **Interop with the desktop:**
  1. Pair the simulator, then a device, with a desktop Compass, on the public server or the local server.
  2. On the desktop, create a pinned folder and tab, then check them on the phone.
  3. On the phone, open a Today tab, then check it on the desktop as an unloaded entry.
  4. Save a password on the phone, then check it on the desktop.
  5. Check history both ways.
  6. Check that the phone appears in the device list and that "Tabs on iPhone" works.
- **Server:** watch the logs for unknown fields or quota errors.

## Risks

- **Protocol drift and hidden engine behavior:** the Swift engine re-implements parts of `components/sync`. To keep it small: only the types listed, server wins on conflicts, and the desktop's apply code as the reference.
- **Nigori details:** the keybag format and key rotation. The fixture tests catch format mismatches, and "Reset sync" on the desktop must send the phone back to the join screen.
- **iOS background limits:** freshness while the app is in the background is best-effort apart from sent tabs, which have a push.
- **Apple review:** an alternative browser is fine with WKWebView. The default-browser entitlement is an optional request.

## Open items

1. **Interop run against the desktop.** Everything is built to the desktop's code and test vectors, and the Swift tests play the desktop's part. A run against a real desktop Compass should check what they can't:
   - a NIGORI made by Chromium opens;
   - the desktop shows the phone's spaces, Today entries, visits and passwords;
   - the phone appears in the desktop's device list and its "Tabs on other devices";
   - with a two-profile Mac: a space moved between profiles on each side, and the phone in each profile's device list.
2. **iPad, on a device:** ⌘W (the simulator keeps it, above), dragging a tab out to a new window (the simulator can't show two windows headless), an extension's `commands` with a hardware keyboard, and Stage Manager windows of every size (the layout rules above were checked in the simulator's full-screen window only).
3. **Not ported from the desktop bridge:** the in-use check when an archive arrives (the desktop keeps an entry in Today if it's shown or playing audio; the phone archives it) and the 100-entry archive cap per space.
4. **Web extensions:** 1Password signed in on a device (Face ID unlock included), a passkey saved and used, and a link's long-press menu with an extension's items.
5. **Background downloads:** POST, `blob:` and `data:` downloads still pause in the background, and the server sees each download's request twice.
