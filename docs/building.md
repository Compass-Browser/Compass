# Building Compass

Compass builds the same way Chromium does, with a thin CLI (`tools/compass`) on
top. If something here is unclear, Chromium's own docs are authoritative:
[mac](https://chromium.googlesource.com/chromium/src/+/main/docs/mac_build_instructions.md),
[windows](https://chromium.googlesource.com/chromium/src/+/main/docs/windows_build_instructions.md),
[linux](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md).

## Hardware

| | Minimum | Comfortable |
|---|---|---|
| Disk | 150 GB free (SSD) | 300 GB+ |
| RAM | 16 GB | 64 GB |
| CPU | 8 cores | 16–32 cores |

A first full build takes roughly 1 hour on a fast 32-core machine, and 4–8
hours on a laptop. Incremental builds after changing Compass code take seconds
to minutes.

**Memory:** some Chromium files need 1–2 GB each to compile. By default the
build runs one job per core, which can exhaust a 16 GB machine badly enough
to freeze the desktop. Limit the parallel jobs to about RAM ÷ 2.5 GB:
`tools/compass build -j 6`, or `export COMPASS_JOBS=6` to make it the default.

## Prerequisites

**macOS**
- Full **Xcode** from the App Store (the Command Line Tools alone aren't
  enough), then `sudo xcode-select -s /Applications/Xcode.app` and
  `sudo xcodebuild -license accept`.
- The Metal Toolchain, which Xcode no longer bundles. ANGLE compiles Metal
  shaders: `xcodebuild -downloadComponent MetalToolchain` (~840 MB).
- The checkout must be on a case-insensitive APFS volume (the default).

**Windows**
- Visual Studio with "Desktop development with C++" and the MFC/ATL
  components. VS 2022 Build Tools works. Chromium prefers VS 2026: if VS 2026
  is installed *without* the C++ workload, `gn gen` fails with
  "vcvarsall.bat is missing". Either add the workload to VS 2026, or point
  Chromium at VS 2022 with the environment variable
  `GYP_MSVS_OVERRIDE_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`.
- The exact Windows SDK version Chromium pins (`SDK_VERSION` in
  `build/toolchain/win/setup_toolchain.py`; 10.0.28000 for Chromium 156),
  including *Debugging Tools for Windows*:
  `winget install --id Microsoft.WindowsSDK.10.0.28000 -e --override "/features OptionId.DesktopCPPx64 OptionId.DesktopCPPx86 OptionId.WindowsDesktopDebuggers /quiet /norestart"`
- `DEPOT_TOOLS_WIN_TOOLCHAIN=0` as a system environment variable, and
  `git config --global core.longpaths true`.
- Git for Windows, and Python 3 (the `py` launcher or `python` on `PATH`;
  `python3` is usually the Microsoft Store stub, which `compass.bat` avoids).
- **Use the in-tree layout.** siso doesn't follow directory junctions
  ("../../compass/BUILD.gn not exist"), so on Windows `compass init`, `gen`
  and `build` refuse the linked layout. Clone the repository straight into
  the checkout and run `init` from there:
  ```bat
  git clone <compass repo> C:\src\compass-chromium\src\compass
  cd C:\src\compass-chromium\src\compass
  tools\compass.bat init
  ```
  The parent directory must be named `compass-chromium`; that's how `init`
  recognizes the layout before Chromium is checked out. An older linked
  checkout can still be converted by hand: remove the
  `src\compass` junction, move the repository there, and run `compass`
  from the new path.

**Linux**
- After `compass init`, run
  `../compass-chromium/src/build/install-build-deps.sh`.

## First build

```sh
tools/compass init          # depot_tools, Chromium at config/chromium_version, patches
tools/compass gen debug     # out/Debug (--cpu arm64|x64: out/Debug-<cpu>)
tools/compass build         # target `chrome`
tools/compass run           # dev profile in out/Debug/compass-dev-profile (--fresh to reset)
```

`compass init` checks out Chromium for the host OS only. To build for another
OS from the same checkout, add `--target-os win` (for example) on the first
init, or edit `../compass-chromium/.gclient` and run `compass sync`.

## Tests

```sh
tools/compass test                                  # compass_unittests
tools/compass test --browser                        # compass_browsertests (opens real windows)
tools/compass test -- --gtest_filter='SpaceModel*'  # arguments go to the test binary
python3 -m unittest discover -s tools/tests        # CLI and build-tooling tests (no checkout needed)
```

## Trying it by hand

The debug build is slow to use. For hands-on testing, build the `fast`
config into out/Fast: optimized, no DCHECKs, still a component build.

```sh
tools/compass gen fast
tools/compass build -c fast
tools/compass run -c fast
```

### A build for another Mac

A component build loads its libraries from the out directory, so its .app
only runs where it was built. The `portable` config is `fast` linked
statically: copy the .app anywhere. To try Compass on an Intel Mac from an
Apple silicon build machine:

```sh
tools/compass gen portable --cpu x64        # out/Portable-x64
tools/compass build -c portable --cpu x64 chrome
```

`--cpu` takes the same value on `build`, `run`, `test` and `sign`. With
`--cpu`, the build gets its own out directory, `out/<Config>-<cpu>` (even for
the host's CPU), so it doesn't overwrite the plain `out/<Config>` build.

## iOS on devices

```sh
tools/compass ios device       # Debug, signed for development, for real devices
tools/compass ios archive --upload   # Release, uploaded to TestFlight
```

`ios device` signs automatically with the team in `ios/project.yml`. To
use your own Apple Developer team, change `DEVELOPMENT_TEAM` there and
`IOS_TEAM` in `tools/compass_tool/cli.py`; bundle IDs belong to one team, so
you'll need your own for those too. The
profile covers every device registered to the team, so the .app
(`ios/build/DerivedData-device/Build/Products/Debug-iphoneos/Compass.app`)
can be copied to another Mac and installed from there:

```sh
xcrun devicectl list devices
xcrun devicectl device install app --device <name|udid> Compass.app
```

On a build machine without an Apple ID in Xcode, both actions take an App
Store Connect API key: `--api-key AuthKey_XXXX.p8 --api-key-id XXXX
--api-issuer <issuer>`.

## Release builds

```sh
tools/compass gen release
tools/compass build -c release chrome
```

### Universal builds (macOS)

`--cpu` on `gen`, `build`, `run` and `test` picks a build for one CPU in its
own directory, `out/<Config>-<cpu>`. Build both and merge them with Chromium's
`chrome/installer/mac/universalizer.py`:

```sh
tools/compass gen release --cpu arm64
tools/compass gen release --cpu x64
tools/compass universal --build -j 6   # out/Release-universal/Compass.app
```

Without `--build`, `universal` only merges the two existing apps. Each CPU is a
full build (on a 10-core, 16 GB M4 at `-j 6`, several hours each).

### Signing and notarization (macOS)

```sh
tools/compass sign             # build, sign, make the .dmg, notarize, staple
tools/compass sign --no-notarize   # sign and package only
```

`compass sign` builds `chrome` and `chrome/installer/mac` in out/Release, then
runs Chromium's signing driver (`out/Release/Compass Packaging/sign_chrome.py`)
with the keychain's "Developer ID Application" certificate (`--identity` picks
one if there are several). The result is `out/Release/signed/Compass-<version>.dmg`.

Notarization uses a `notarytool` keychain profile, `compass` by default
(`--keychain-profile`). Store it once per Mac, with an app-specific password
from account.apple.com or an App Store Connect API key:

```sh
xcrun notarytool store-credentials compass --apple-id <APPLE_ID> --team-id <TEAM_ID>
```

The driver submits the app and then the .dmg, waits for Apple, staples both
tickets, and `compass sign` checks the .dmg with `spctl`.

## Troubleshooting

- **A change to an override isn't picked up:** run `compass build` rather than
  plain `autoninja`. `compass build` touches upstream files when overrides are
  added or removed.
- **`gn gen` says an argument has no effect:** Chromium renamed or removed
  it. Update `config/args/*.gn`.
- **A Chromium build script rejects a path under `src/compass`:** some
  scripts resolve symlinks. Switch to the in-tree layout: clone this
  repository directly into `../compass-chromium/src/compass`. The CLI detects
  it automatically.
