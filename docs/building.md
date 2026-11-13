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
  `build/toolchain/win/setup_toolchain.py`; 10.0.28000 for Chromium 157),
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

The app is signed with `com.apple.application-identifier`
(`<TEAM_ID>.it.cipi1965.compass`). Without it macOS doesn't hand Compass
the browser logins other apps start with `ASWebAuthenticationSession`
(Spark's "Sign in with Google", for example): SafariLaunchAgent logs "Failed
to get application identifier" and the login never opens. The entitlement
needs a Developer ID provisioning profile for the App ID inside the app, so
`compass sign` embeds
`~/Library/Application Support/Compass Release/Compass.provisionprofile`
(`--provisioning-profile` for another file). It refuses a profile for
another App ID, an expired one, or a development profile. Without one it
signs without the entitlement and warns (`--no-provisioning-profile`
silences the warning). The profile is never in the repository. Development
and test builds never get the entitlement, since macOS won't launch an app
that has it without a matching profile.

### Windows from a Mac

A Mac can build Windows Compass (Chromium's `docs/win_cross.md`): `--cpu
win-x64` builds it in `out/<Config>-win-x64`. Setting it up, once per Mac:

1. Add `"win"` to `target_os` in the Chromium checkout's `.gclient` (e.g.
   `target_os = ["mac", "win"]`) and run `gclient sync --nohooks
   --no-history` there, for the Windows-only dependencies.
2. Package the Windows toolchain on a Windows machine, with the Visual
   Studio and SDK that `build/vs_toolchain.py` names (`MSVS_VERSIONS`,
   `SDK_VERSION`: for Chromium 157, Visual Studio 2026 with its C++ workload,
   MSVC v145 and ATL, and SDK 10.0.28000.0; the script refuses older Visual
   Studios, so a machine that builds with VS 2022 Build Tools can't make it):

   ```bat
   cd C:\src\compass-chromium\depot_tools\win_toolchain
   py -3 package_from_installed.py 2026 -w 10.0.28000.0
   ```

   Keep the ARM64 libraries (no `--noarm`): on an Apple silicon Mac the
   host toolchain's data is ARM64's, and GN checks it even for x64 builds.
   The x86 ones are checked too.

   Put the resulting `<hash>.zip` (and nothing else) in `win_toolchain/` next
   to the checkout's `depot_tools/`. The compass tool then sets
   `DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL` and `GYP_MSVS_HASH_<TOOLCHAIN_HASH>`
   for gclient and the build.
3. `gclient runhooks` in the checkout with those two variables set (as
   `tools/compass sync` does), which unpacks the toolchain into
   `depot_tools/win_toolchain/vs_files`.

Then:

```sh
tools/compass gen release --cpu win-x64
tools/compass build -c release --cpu win-x64 chrome mini_installer
tools/compass release build-win --cpu win-x64   # the release installers
```

Limits: Crashpad can't report crashes from a cross build (Chromium stubs
out its `.asm`), and there's no Authenticode signature either way. A
Windows release build's `compass_win_build` lives in `chrome/VERSION`,
which the checkout's Mac builds share, so switching between a Windows
release and a Mac build rebuilds what reads the version on both sides
(docs/design/updates.md, "Installer versions"). Keep a separate checkout
for Windows builds.

### Android from a Mac

The Android app is built from the same checkout (docs/design/android.md):
`--cpu android-arm64` builds it in `out/<Config>-android-arm64`.
`--cpu android-x64` builds it for the Android emulator on an x86_64 host
(Windows or Intel Mac), in `out/<Config>-android-x64`: the emulator's ARM
translation crashes the arm64 build at startup. Chromium
only supports Linux hosts for Android, and calls a Mac host "best-effort".
On Apple silicon it does work, because Chromium's CIPD packages already come
in Mac flavors: the JDK, the SDK build tools, the NDK (`darwin-x86_64`, run
through Rosetta) and clang's Android runtimes.

Set up once per checkout:

1. Add `"android"` to `target_os` in the checkout's `.gclient` (e.g.
   `target_os = ["mac", "android"]`) and run `tools/compass sync`, which
   fetches the Android dependencies (tens of GB).
2. Make sure Rosetta is installed (`softwareupdate --install-rosetta`). Some
   of the NDK's tools are x86-64.

Then:

```sh
tools/compass gen portable --cpu android-arm64
tools/compass build -c portable --cpu android-arm64 compass_apk
```

What makes the Mac host work:

- **Patches** (they only matter on Android builds from a Mac):
  - `build-config-BUILDCONFIG.gn.patch` lets the Android toolchain run on
    a Mac host;
  - `BUILD.gn.patch` skips the `optimize_gn_gen` group, which preloads
    Linux host toolchains only to speed up `gn gen`;
  - `build-config-android-config.gni.patch` lists the JDK's `.so` files as
    build inputs only on a Linux host (the Mac JDK has `.dylib`s).
- **Fix-ups** that `compass gen` and `compass build` apply to the checkout
  after every sync (`tools/compass_tool/android.py`):
  - the Mac JDK is a bundle, so its `Contents/Home/*` is linked into
    `third_party/jdk/current`;
  - Chromium's pinned `aapt2` is Linux only, so it's replaced with a link
    to the SDK build tools' Mac `aapt2`.
- **A GN arg** (`config/args/android.gn`): `v8_use_host_cpu_arm_features =
  false`. On an Apple silicon host V8's `mksnapshot` runs natively, and by
  default it lets the builtins it writes use the instructions its compiler
  targets: the Mac's, which many Android CPUs lack (pages die of SIGILL on
  their first JavaScript). On Linux it runs through a simulator, and the
  arg doesn't build there: `compass gen` on Linux sets it back to `true`
  in args.gn, after the import.

Limits:

- **No emulator from the checkout.** `adb` and the emulator under
  `third_party/android_sdk` are Linux builds. Use Google's Mac
  platform-tools for `adb`, and a Genymotion SaaS arm64 device or the
  Android Studio emulator to run the APK.
- **Slow first build.** A full build of stock `chrome_public_apk` (portable
  config) took 8h41m on a Mac mini running other jobs. Builds after small
  changes are incremental.

### Linux from Windows

Experimental. Chromium builds Linux only on Linux hosts, but nothing in GN
forbids a Windows host, and with the patches below a Windows machine builds
a working Linux x64 Compass: it starts under WSL2 and loads pages. `--cpu
linux-x64` builds it in `out/<Config>-linux-x64`; the dependencies, sysroot
and toolchain below are set up by hand.

Set up once per checkout:

1. **Linux dependencies.** The `checkout_linux` git dependencies in `DEPS`
   (fontconfig, wayland, libdrm, pipewire, ...) at their pinned revisions,
   with `core.symlinks=true`: a few headers are git symlinks (libsync's
   `sync/sync.h`), which Git for Windows otherwise checks out as text files.
   Adding `"linux"` to `.gclient`'s `target_os` and syncing does this too.
2. **Sysroot.** Chromium's Debian sysroot
   (`build/linux/sysroot_scripts/sysroots.json`, `bullseye_amd64`) in
   `build/linux/debian_bullseye_amd64-sysroot`, unpacked with real symlinks
   (an elevated shell can make them). Nine kernel headers differ from
   others only in case (netfilter's `xt_CONNMARK.h` and `xt_connmark.h`);
   Chromium uses neither.
3. **Toolchain** in `src/third_party/compass-linux-llvm`. It has to be
   inside `src/`, because siso refuses paths outside it. Start from a copy
   of `third_party/llvm-build/Release+Asserts`, then add:
   - `clang.exe` and `clang++.exe` (copies of `clang-cl.exe`), and
     `ld.lld.exe` (a copy of `lld-link.exe`): the binaries pick their mode
     from their name;
   - `llvm-nm.exe` from Chromium's `Win/llvmobjdump-<clang revision>.tar.xz`;
   - `llvm-ar`, `llvm-readelf`, `llvm-objcopy` and `llvm-strip` from an
     official LLVM Windows release (Chromium doesn't ship them for Windows);
   - `lib/clang/<version>/lib/x86_64-unknown-linux-gnu` from Chromium's
     `Linux_x64/clang-<clang revision>.tar.xz` (the Linux runtimes).

Then:

```bat
tools\compass.bat gen portable --cpu linux-x64
tools\compass.bat build -c portable --cpu linux-x64 chrome
```

`config/args/linux.gn` points `clang_base_path` at the toolchain and turns
off what a Windows host can't build: CUPS and remoting (`gn gen` runs their
shell scripts) and the V8 context snapshot (below). `gen` applies the
patches first, since `gn gen` already needs them.

What makes the Windows host work, all no-ops for other builds:

- `build-toolchain-win-toolchain.gni.patch`,
  `build-toolchain-win-BUILD.gn.patch`: the Windows toolchains, which a
  Windows host uses as its host toolchains, assert `is_win`; they're
  loaded with a Linux default toolchain here.
- `build-toolchain-gcc_toolchain.gni.patch`: the static-library step runs
  `//compass/build/toolchain/ar_wrapper.py` instead of a `cmd /s /c` line
  that cmd.exe misreads under siso.
- `BUILD.gn.patch`, `chrome-BUILD.gn.patch`: no breakpad host tools and no
  Linux symbol files. On a Windows host breakpad builds its PDB `dump_syms`,
  which can't read ELF.
- `third_party-wayland-wayland_protocol.gni.patch`,
  `third_party-wayland-BUILD.gn.patch`: `wayland_scanner` runs as
  `wayland_scanner.exe`, and builds with the `getopt.h` and `unistd.h` in
  `//compass/build/win_host_compat`.
- `third_party-lzma_sdk-BUILD.gn.patch`: lzma's MASM files only for a
  Windows target (upstream checks only the host).
- `v8/src-wasm-wasm-objects.cc.patch`: `sys/mman.h` only when the host is
  Linux too (upstream checks only the target, and `mksnapshot` is built for
  the host).

Limits:

- **No V8 context snapshot.** Its generator runs on the host, so it would
  build all of Blink a second time for Windows (and its path lacks
  `.exe`). Without it, each new page sets up its JavaScript context at
  start, a little slower.
- **No printing** to system printers (`use_cups = false`; Save as PDF
  works), **no crash symbols**, **no `.deb`/`.rpm`** yet.
- **Running it** takes a Linux machine or WSL2.

#### Linux packages

```
tools\compass.bat package --cpu linux-x64 --build --flatpak
```

`--build` builds `chrome` and `chrome_sandbox` first. `package` then writes
two files into `out\Portable-linux-x64`, named after the release version
(`--version`, by default date.time as for releases):

- `Compass-<version>-linux-x64.tar.xz`: the browser with symbols stripped,
  plus a `compass` launcher, a `.desktop` file, the icons and a README. The
  Unix modes are set by `tools/compass_tool/linux_package.py`, since NTFS
  doesn't record them. `chrome-sandbox` is setuid root, for distributions
  that restrict user namespaces (Ubuntu 24.04 and later). Unpacking as a
  regular user drops that bit, and the README says how to restore it.
- `Compass-<version>-linux-x64.flatpak`: a Flatpak bundle made from the
  tarball by `installer/linux/flatpak/io.github.Compass_Browser.Compass.yml`.
  It's built with flatpak-builder in WSL (`--wsl-distro`, default
  Ubuntu-24.04). It runs on the Freedesktop 25.08 runtime with Chromium's
  BaseApp, whose zypak sandboxes the renderers through Flatpak instead of
  `chrome-sandbox`. Install it with `flatpak install --user <file>`. The
  same manifest can go to Flathub later, with the release URL and sha256 as
  its source.

Once in WSL, as root:

```
apt install flatpak flatpak-builder
flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.freedesktop.Sdk//25.08 org.chromium.Chromium.BaseApp//25.08
```

The browser needs glibc 2.25 or later. It also needs these libraries
(Debian package names): libglib2.0-0, libnss3, libnspr4, libatk1.0-0,
libatk-bridge2.0-0, libatspi2.0-0, libdbus-1-3, libexpat1, libudev1,
libx11-6, libxext6, libxcomposite1, libxdamage1, libxfixes3, libxrandr2,
libxcb1, libxkbcommon0, libcairo2, libpango-1.0-0, libgbm1 and libasound2.
It uses GTK 3 or 4, PulseAudio or PipeWire, libva and Vulkan when they're
installed.

`package` also writes, next to the tarball:

- `Compass-<version>-linux-x64.oci`: the same Flatpak as an OCI image (a
  folder), for the remote below;
- `Compass-<version>-linux-x64.json`: the version, Chromium version, channel
  (`--channel`, default canary) and commit, for `release publish-linux`.

The Flatpak's branch is the channel (`canary`), so a later stable one installs
next to it, and the bundle names the remote below as where it updates from.

#### Linux releases

```
tools/compass release publish-linux --tarball Compass-<version>-linux-x64.tar.xz
```

Everything is on GitHub; the macOS and Windows update feeds aren't touched.

- **The GitHub prerelease** `v<version>` gets the tarball, the bundle, the
  Flatpak's layer (`…oci-layer.tar.gz`, the browser itself) and
  `compass-canary.flatpakref`.
- **The Flatpak remote** is `oci+https://raw.githubusercontent.com/Compass-Browser/Compass/flatpak/`,
  the repository's `flatpak` branch (`tools/compass_tool/flatpak_remote.py`).
  An OSTree repository is a tree of small files, which release assets can't
  hold, so it's an OCI remote: Flatpak reads `index/static` (the current
  image of each branch, with its ref and metadata), then the image's manifest
  and config under `v2/compass/`, all a few KB on that branch, then the layer
  from the URL its manifest lists, the release asset. Each release adds one
  commit to the branch, through the GitHub API. Flatpak checks everything
  against its digest; OCI remotes have no GPG signatures, so the index's
  integrity is GitHub's HTTPS.
- **Installing:**
  `flatpak install https://raw.githubusercontent.com/Compass-Browser/Compass/flatpak/compass-canary.flatpakref`
  (the Freedesktop runtime and Chromium's BaseApp come from Flathub). Or
  install the bundle: it adds the same remote.
- **Updates** come with `flatpak update` and the software centers' automatic
  updates. A running Compass keeps its version until it's restarted.
- `--no-upload` lists what it would publish. Publishing needs `gh` signed in
  with write access to the repository.

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
