# Automatic updates

Status: macOS done (Sparkle, both channels, `compass release`); Windows
written and unit-tested here, but not yet built or run on Windows
(`compass_enable_win_updater` is off). Linux: not planned (distro packages
or Flatpak).

## Channels

Two channels, **stable** and **beta**, on the same app (one bundle ID, one
profile). The user picks one on the About page (chrome://settings/help,
"Update channel"); it's the Local State pref `compass.update_channel`
(`"stable"` by default).

- Every release goes to **beta** first. **Promoting** it to stable later
  re-labels the same archive: nothing is rebuilt, so stable is always a beta
  build that held up.
- Beta users get stable releases too, whenever one is newer than the newest
  beta.
- Moving from beta back to stable never installs an older version: the app
  stays on its beta build until stable passes it.

## Versions

Compass has its own version, `YYYYMMDD.HHMM` (e.g. `20261103.0215`), like
the iOS build numbers. `compass release` picks it at build time and sets
`compass_version` in `out/Release/args.gn`. Chromium's version stays as it
is and is still shown.

- macOS: `CFBundleVersion` and `CompassVersion` in the app's Info.plist hold
  it (`browser/updates/stamp_app_plist.py`, run by `compass_app_plist` in
  `chrome/BUILD.gn`). Sparkle compares `CFBundleVersion`.
  `CFBundleShortVersionString` stays Chromium's, because Chromium reads it.
- Everywhere: `compass::updates::CompassVersion()` (the `COMPASS_VERSION`
  buildflag). It's empty in dev builds, which never update.

## Server

`https://updates.compassbrowser.org` runs on the server (<server>, Debian 13),
behind the host's Caddy container (`/opt/docker/caddy`, site file
`config/sites/compassbrowser.caddy`, a copy of `website/compassbrowser.caddy`).
compassbrowser.org is proxied by Cloudflare, and its certificates come
through HTTP-01 (the host's Cloudflare token doesn't cover it). Until
2026-11-07 the server was `https://updates.compassbrowser.org`; that name is
baked into installed copies (Sparkle's `SUFeedURL`), so it still serves the
same files, and the Windows updater accepts installers from either name.
The homepage, `https://compassbrowser.org` (`website/`, rsynced to
`/var/www/compassbrowser.org`), reads `stable.json` and `beta.json` for its
download links (the feeds send `Access-Control-Allow-Origin: *`). It's a
static file server for `/var/www/compass-updates`:

| Path | What |
|---|---|
| `releases.json` | Every release: version, channel, commit, Chromium version, date, and per platform the file, size, SHA-256 and Ed25519 signature (plus macOS deltas). The source of truth. |
| `stable.json`, `beta.json` | The newest release each channel gets, per platform (`mac`, `win`): version, URL, size, SHA-256, signature. The Windows updater reads these. |
| `mac/appcast.xml` | Sparkle's feed, both channels (beta items carry `<sparkle:channel>beta</sparkle:channel>`). |
| `mac/Compass-<version>.dmg` | Notarized, stapled disk images. |
| `mac/Compass-<from>-<to>.delta` | Sparkle binary deltas from the three previous releases. |
| `win/CompassSetup-<version>.exe` | Chromium's mini_installer. |

Feeds are served `no-cache`, and archives `immutable`. Uploads go through a
system user, `compass-updates`, whose only key (`~/.ssh/compass_updates_deploy`
on the Mac mini) is restricted to `rrsync /var/www/compass-updates`. That
key isn't in 1Password and needs no agent, so unattended releases work.

## Signing key

Sparkle's Ed25519 key signs every archive and delta. The Windows updater
checks the same signatures.

- Private key: `~/Library/Application Support/Compass Release/sparkle_ed25519.key`
  on the Mac mini (also in its login keychain, account `compass-updates`).
  **It must be backed up** (1Password): without it, installed copies can't
  verify any later update.
- Public key: `compass_sparkle_public_key` in `config/args/release.gn`,
  which ends up in Info.plist (`SUPublicEDKey`) and the `COMPASS_UPDATE_PUBLIC_KEY`
  buildflag.

## Releasing

```sh
tools/compass release                  # = publish --channel beta: build, sign,
                                       #   notarize, deltas, upload, feeds
tools/compass release promote 20261103.0215   # that release goes to stable
tools/compass release list
tools/compass release feeds            # rewrite and upload the feeds
tools/compass release publish-win 20261103.0215 --installer mini_installer.exe
```

`publish` uploads the archive and deltas first and the feeds last, so a
feed never names a missing file. The local mirror of the site lives in
`~/Library/Application Support/Compass Release/site` (it keeps old .dmg
files, for deltas).

## macOS: Sparkle

Sparkle 2 (`tools/compass_tool/sparkle.py` pins the release and checks its
SHA-256). `compass build` fetches it into `third_party/sparkle/` (gitignored):
`sdk/` has the full framework for its headers, `bundle/` the copy that ships
(without headers, modules or XPC services, which only sandboxed apps need),
and `bin/` the tools.

- **Bundling:** release builds (`compass_update_feed_url` set) copy
  `Sparkle.framework` into `Compass.app/Contents/Frameworks`
  (`//compass/browser/updates:sparkle_framework`, a `bundle_deps` of
  `chrome_app`). Chromium's signing driver signs `Autoupdate`, `Updater.app`
  and the framework before the app (`signing/parts.py` patch). Dev builds
  have no framework and no feed, so nothing starts.
- **Loading:** `browser/updates/sparkle_updater_mac.mm` loads the framework
  at run time (`NSBundle`), so nothing links against it, and starts an
  `SPUUpdater` from `PostBrowserStart()`. Info.plist sets automatic checks
  (every 4 hours), automatic downloads, and no system profile.
- **No Sparkle UI:** a silent user driver, so Sparkle never shows a window.
  Its delegate picks the channels (`allowedChannelsForUpdater:`) and takes
  over installation (`willInstallUpdateOnQuit:` returns YES and keeps the
  handler).
- **Chromium's UI:** once an update is downloaded, Compass tells upstream's
  `BuildState` about it. Upstream's upgrade detector compares Chromium
  versions, so when only Compass changed it reports the running version
  plus `.1`, and `updater::CurrentlyInstalledVersion()` returns the same
  value (`updater_no_updater.cc` patch), so the poller doesn't reset it. The
  app menu then offers "Relaunch to update", as in Chrome.
- **Relaunch:** `AttemptRestartInternal()` asks `compass::updates::Updater`
  first (`application_lifetime_desktop.cc` patch). With an update downloaded,
  Sparkle installs it and opens the app again; Chromium sets `kWasRestarted`
  so the session comes back, but doesn't start its own relauncher. Quitting
  normally installs the update as well, without reopening the app.
- **About page:** `version_updater_mac.mm` (patch) reports Sparkle's checks
  (checking, downloading with progress, relaunch, up to date, failed). The
  channel row shows Compass's version (`compassVersion`) and the channel
  (`about_page.*` patches; `compass.update_channel` is allowlisted in
  `prefs_util.cc`). Changing the channel checks at once.
- **Testing:** `--compass-check-for-updates` checks at launch. Add
  `--enable-logging=stderr --vmodule=sparkle_updater_mac=1` for the
  `Compass updates:` log lines. Test with a copy on the internal disk
  (e.g. `~/CompassUpdateTest/Compass.app`, its own `--user-data-dir` with
  `{"compass":{"update_channel":"beta"}}` in Local State), started with
  `open -n -g -a … --args …`, and quit it with an Apple Event to its PID
  (`NSRunningApplication … terminate`). Two traps found on 2026-11-03: a
  copy started from a shell makes the shell its responsible process, so
  macOS's App Management check stops Sparkle's swap of the bundle
  (`renamex_np` waits on a prompt); and a copy on the external disk started
  by LaunchServices waits on the removable-volume prompt at launch.
  Installed copies started from the Dock or Finder meet neither.
- **Checked on 2026-11-03:** 20261103.0116 → 20261103.0132 on the beta
  channel, through a 3.7 MB delta, installed on quit; then 20261103.0132 →
  20261103.0156 through a 0.27 MB delta, from the About page's "Nearly up
  to date! Relaunch" (Sparkle installed it and opened the app again). Both
  results pass `codesign --verify --deep --strict` and Gatekeeper. The
  channel row switches the pref. 20261103.0156 was promoted to stable and
  installed in /Applications.
- **Relaunch caveat:** Sparkle reopens the app without its command line, so
  a test copy with its own `--user-data-dir` comes back on the default
  profile.

## Windows

Chromium's own installer does the installing. A small updater in the
browser decides when to run it.

- **Installer:** `mini_installer.exe` (per-user, `%LOCALAPPDATA%\Compass`).
  A newer one updates an installed copy in place. When the browser is
  running, setup.exe leaves `new_chrome.exe` and the new version folder.
  Upstream's installed-version monitor sees the new version in the registry
  (`pv`), the app menu offers to relaunch, and the relaunch swaps the
  executable. That's all upstream behaviour; nothing is patched for it.
- **Updater** (`browser/updates/win_updater.cc`, only with
  `compass_enable_win_updater = true`): two minutes after launch, then every
  4 hours, when the channel changes, and from the About page, it reads
  `<channel>.json`. When `win.version` is newer than `CompassVersion()`, it
  downloads the installer (only https from the update server), checks its
  size, SHA-256 and Ed25519 signature (`update_manifest.cc`, unit-tested
  against a signature from Sparkle's own `sign_update`), and runs it with
  `--do-not-launch-chrome`. The About page goes through
  `version_updater_basic.cc` (patch).
- **To ship it:** on the Windows machine, set `compass_version` (the same
  version as the Mac release, or a new one) and
  `compass_enable_win_updater = true` in `out/Release/args.gn`, then build
  `mini_installer`. On the Mac mini, run `compass release publish-win
  <version> --installer mini_installer.exe [--chromium <version>]`.
- **Not done:** no Authenticode signature (no certificate), so SmartScreen
  warns about the first install's download. Updates are fine: the updater
  runs the file itself, with no mark of the web. No differential updates
  (mini_installer is about 120 MB). The updater hasn't been built or run on
  Windows yet.

## Not done

- Phased rollouts (`sparkle:phasedRolloutInterval`) and critical updates
  (the manifest has `critical`, nothing sets it).
- Release notes: `--notes` is stored, but neither feed shows it.
- A download page: `stable.json` has the URLs, but nothing on the server
  redirects to the newest installer.
