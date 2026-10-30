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
- Visual Studio 2022 with "Desktop development with C++", the MFC/ATL
  components, and the Windows 11 SDK, including *Debugging Tools for Windows*.
- Git for Windows, and Python 3 on `PATH`.
- Developer Mode isn't required: `//compass` is linked into the checkout with
  a directory junction.

**Linux**
- After `compass init`, run
  `../compass-chromium/src/build/install-build-deps.sh`.

## First build

```sh
tools/compass init          # depot_tools, Chromium at config/chromium_version, patches
tools/compass gen debug     # out/Debug (add --cpu arm64|x64 to cross-compile)
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

## Release builds

```sh
tools/compass gen release
tools/compass build -c release chrome
```

Signing, notarization and installers are set up in CI, not locally.

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
