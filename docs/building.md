# Building Strata

Strata builds the same way Chromium does, with a thin CLI (`tools/strata`) on
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
hours on a laptop. Incremental builds after changing Strata code take seconds
to minutes.

**Memory:** some Chromium files need 1–2 GB each to compile. By default the
build runs one job per core, which can exhaust a 16 GB machine badly enough
to freeze the desktop. Limit the parallel jobs to about RAM ÷ 2.5 GB:
`tools/strata build -j 6`, or `export STRATA_JOBS=6` to make it the default.

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
- Developer Mode isn't required: `//strata` is linked into the checkout with
  a directory junction.

**Linux**
- After `strata init`, run
  `../strata-chromium/src/build/install-build-deps.sh`.

## First build

```sh
tools/strata init          # depot_tools, Chromium at config/chromium_version, patches
tools/strata gen debug     # out/Debug (add --cpu arm64|x64 to cross-compile)
tools/strata build         # target `chrome`
tools/strata run           # dev profile in out/Debug/strata-dev-profile (--fresh to reset)
```

`strata init` checks out Chromium for the host OS only. To build for another
OS from the same checkout, add `--target-os win` (for example) on the first
init, or edit `../strata-chromium/.gclient` and run `strata sync`.

## Tests

```sh
tools/strata test                                  # strata_unittests
tools/strata test --browser                        # strata_browsertests (opens real windows)
tools/strata test -- --gtest_filter='SpaceModel*'  # arguments go to the test binary
python3 -m unittest discover -s tools/tests        # CLI and build-tooling tests (no checkout needed)
```

## Release builds

```sh
tools/strata gen release
tools/strata build -c release chrome
```

Signing, notarization and installers are set up in CI, not locally.

## Troubleshooting

- **A change to an override isn't picked up:** run `strata build` rather than
  plain `autoninja`. `strata build` touches upstream files when overrides are
  added or removed.
- **`gn gen` says an argument has no effect:** Chromium renamed or removed
  it. Update `config/args/*.gn`.
- **A Chromium build script rejects a path under `src/strata`:** some
  scripts resolve symlinks. Switch to the in-tree layout: clone this
  repository directly into `../strata-chromium/src/strata`. The CLI detects
  it automatically.
