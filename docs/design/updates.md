# Automatic updates

macOS updates through Sparkle, on both channels, published with
`compass release`. Windows updates through Compass's own updater over
Chromium's installer, with installers built on Windows or cross-built on a
Mac. Linux: not planned (distro packages or Flatpak).

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

## Where releases live

The **feeds** are on `https://updates.compassbrowser.org`, the URL installed
copies poll. The **archives** (disk images, deltas, Windows installers) are
assets of Compass's GitHub releases, one per version, tagged `v<version>`:

```
https://github.com/Compass-Browser/Compass/releases/download/v<version>/<file>
```

Beta releases are GitHub prereleases; promoting one to stable makes it the
latest release. Each archive entry in `releases.json` carries its `url`.
Releases made before the archives moved to GitHub have no `url`; theirs stay
on the update server, at `https://updates.compassbrowser.org/<file>`.

## Update server

`https://updates.compassbrowser.org` is a static file server behind
Cloudflare. Feeds are served with `Cache-Control: no-cache`; archives and
deltas never change once published, so they're cached for a year. The
homepage, `https://compassbrowser.org` (`website/`), reads `stable.json` and
`beta.json` for its download links (the feeds send
`Access-Control-Allow-Origin: *`).

| Path | What |
|---|---|
| `releases.json` | Every release: version, channel, commit, Chromium version, date, and per platform the file, URL, size, SHA-256 and Ed25519 signature (plus macOS deltas). The source of truth. |
| `stable.json`, `beta.json` | The newest release each channel gets, per platform (`mac`, `win`): version, URL, size, SHA-256, signature, and for Windows the windowed installer's URL, size and SHA-256 (`win.installer`). The Windows updater reads these. |
| `mac/appcast.xml` | Sparkle's feed, both channels (beta items carry `<sparkle:channel>beta</sparkle:channel>`). |
| `mac/Compass-<version>.dmg` | Notarized, stapled disk images (older releases; newer ones are on GitHub). |
| `mac/Compass-<from>-<to>.delta` | Sparkle binary deltas from the three previous releases (likewise). |
| `win/CompassSetup-<version>.exe` | Chromium's mini_installer, which the Windows updater runs (likewise). |
| `win/CompassInstaller-<version>.exe` | The installer with a window that the website offers (below; likewise). |

Uploads go over rsync and SSH. The target isn't in the repository: the
uploader reads it from `~/Library/Application Support/Compass Release/upload.json`,

```json
{"host": "user@host", "port": 22, "key": "/path/to/ssh_key"}
```

The key should be a dedicated one, restricted on the server to the site's
directory (e.g. with `rrsync`), and need no agent, so unattended releases
work.

GitHub uploads go through the GitHub CLI: the release machine needs `gh`,
signed in (`gh auth login`) as an account that can write releases to
`Compass-Browser/Compass`.

## Signing key

Sparkle's Ed25519 key signs every archive and delta. The Windows updater
checks the same signatures.

- Private key: `~/Library/Application Support/Compass Release/sparkle_ed25519.key`
  on the release machine (also in its login keychain). **It must be backed
  up**: without it, installed copies can't verify any later update.
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

`publish` creates the version's GitHub release, uploads the archive and
deltas to it, and only then uploads the feeds, so a feed never names a
missing file. The release tags the commit it was built from when GitHub has
that commit, and the default branch otherwise. `publish-win` adds the
installer to the same release (or creates it), and `promote` also makes the
GitHub release the latest, non-prerelease one. The local mirror of the site lives in
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
  (`NSRunningApplication … terminate`). Two traps: a
  copy started from a shell makes the shell its responsible process, so
  macOS's App Management check stops Sparkle's swap of the bundle
  (`renamex_np` waits on a prompt); and a copy on the external disk started
  by LaunchServices waits on the removable-volume prompt at launch.
  Installed copies started from the Dock or Finder meet neither.
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
- **Updater** (`browser/updates/win_updater.cc`, with
  `compass_enable_win_updater = true`, which release.gn sets): two minutes after launch, then every
  4 hours, when the channel changes, and from the About page, it reads
  `<channel>.json`. When `win.version` is newer than `CompassVersion()`, it
  downloads the installer (only over https, from the update server or
  `github.com/Compass-Browser/Compass/releases/download/`), checks its
  size, SHA-256 and Ed25519 signature (`update_manifest.cc`, unit-tested
  against a signature from Sparkle's own `sign_update`), and runs it with
  `--do-not-launch-chrome`. The About page goes through
  `version_updater_basic.cc` (patch).
- **Installer versions:** setup.exe only updates in place when Chromium's
  version goes up; with the same version it "repairs" the install, which
  stages nothing. Two Compass releases often share a Chromium version, so
  each Windows release gets the next **Windows build number** N, and its
  installer is Chromium `MAJOR.N.BUILD.PATCH` (e.g. 156.3.8078.48). N is the
  GN arg `compass_win_build`; `compass build` writes it into
  `chrome/VERSION`'s `MINOR` (many targets read that file directly) and puts
  0 back for builds without it. `releases.json` records it as `win.build`,
  and `publish-win` refuses a number that isn't newer than every earlier
  one. chrome://version and the full-version client hints show N; the
  reduced user agent (`156.0.0.0`) doesn't.
- **Rebuilds:** official builds salt Mojo's message IDs with
  `chrome/VERSION`, so a new N would regenerate every binding and recompile
  most of Chromium. Builds with an N salt with upstream's version instead
  (`compass/build/mojom_message_id_salt`, written by `compass build`, ignored
  by git), which still changes with every Chromium update. What still reads
  `chrome/VERSION` is the version headers and resources, a few dozen build
  rules instead of thousands.
- **To ship it:** on the Windows machine, `tools\compass.bat release
  build-win [<version>]` (the Mac release's version, or a new one) reads
  `releases.json` from the server for the next N, sets `compass_version`,
  `compass_win_build` and `compass_enable_win_updater` in
  `out/Release/args.gn`, builds `chrome mini_installer compass_installer`,
  and leaves `CompassSetup-<version>.exe`, `CompassInstaller-<version>.exe`
  and a `.json` (version, Chromium, N, commit, installer) in the out dir.
  Copy them to the release machine and run `compass release publish-win
  --installer CompassSetup-<version>.exe [--channel beta]`. A
  Mac can build it too (`--cpu win-x64`, docs/building.md "Windows from a
  Mac"), in a Chromium checkout of its own so the build number doesn't make
  its Mac builds rebuild.
- **First install:** mini_installer has no window: run by hand, it
  installs and opens Compass with nothing in between. The website offers
  `CompassInstaller-<version>.exe` instead (`installer/win`): a small window
  with Compass's icon, "Installing Compass…" and a marquee bar, around the
  mini_installer it carries as a resource. It runs that with
  `--do-not-launch-chrome`, then opens Compass from
  `%LOCALAPPDATA%\Compass\Application` (so a repair of the same version
  also ends with Compass open), or says what failed (the installer's exit
  code; the log is `chromium_installer.log` in Temp). There's no Cancel:
  mini_installer can't be stopped safely halfway. `build-win` builds it with
  the rest and names it in its `.json`; `publish-win` uploads it next to
  CompassSetup and lists it as `win.installer` in the feeds, which the
  website's Windows links use. The updater keeps running mini_installer.
- **After an update:** the installer stages `new_chrome.exe` and sets the
  registry's `pv`; the About page offers Relaunch at once. The app menu's
  "Relaunch to update" comes at upstream's first notification stage, an
  hour after the upgrade detector sees the update
  (`kDefaultVeryLowThreshold`, checked every 20 minutes). setup.exe leaves
  the old version folder until its next run.
- **Not done:** no Authenticode signature (no certificate), so SmartScreen
  warns about the first install's download (either installer), and Windows
  11's Smart App Control, where it's on, blocks the installers outright
  (Code Integrity events 3033/3077). CompassInstaller then says so, from
  CreateProcess's ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION. Updates are fine: the updater
  runs the file itself, with no mark of the web. No differential updates
  (mini_installer is about 130 MB).

## Not done

- Phased rollouts (`sparkle:phasedRolloutInterval`) and critical updates
  (the manifest has `critical`, nothing sets it).
- Release notes: `--notes` is stored, but neither feed shows it.
- A download page: `stable.json` has the URLs, but nothing on the server
  redirects to the newest installer.
