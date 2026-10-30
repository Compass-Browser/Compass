# Compass

An open-source, cross-platform browser built on Chromium, with an Arc-style
sidebar: spaces, profiles, pinned tabs, nested folders, split view, window
sync and automatic picture-in-picture.

> Early scaffold. Nothing here has been built against Chromium yet. See
> [Status](#status).

- **Design:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **Building:** [docs/building.md](docs/building.md)
- **Updating Chromium:** [docs/rebasing.md](docs/rebasing.md)
- **Changing upstream code:** [chromium_src/README.md](chromium_src/README.md), [patches/README.md](patches/README.md)

## Quick start

```sh
# Prerequisites: see docs/building.md (Xcode / Visual Studio / build deps).
tools/compass init            # depot_tools + Chromium at config/chromium_version (~1 h, ~60 GB)
tools/compass gen debug       # out/Debug with Compass's GN args
tools/compass build           # build Compass (first build: hours)
tools/compass run             # launch with a dev profile (--fresh to reset it)
tools/compass test            # build and run compass_unittests
```

The Chromium checkout goes in `../compass-chromium` next to this repository.
Set `COMPASS_CHROMIUM_DIR` to put it somewhere else.

## Repository layout

| Path | What |
|---|---|
| `browser/` | Compass's C++ browser code (`//compass/browser`) |
| `chromium_src/` | Compile-time overrides of upstream files |
| `patches/` | Minimal patches to upstream files, one per file |
| `config/` | Pinned Chromium version and GN args |
| `build/` | Build integration (`redirect_cc.py`) |
| `tools/` | The `compass` CLI and its tests |
| `docs/` | Contributor docs |

## Status

Done in the scaffold:

- [x] `compass` CLI: init, sync, patches, gen, build, run, test, update-chromium
- [x] `chromium_src` override mechanism (compiler wrapper), with tests
- [x] Patches: branding, wiring `//compass/browser` into Chrome, `compass_unittests` in `gn_all`
- [x] `SpaceModel`: sidebar tree with folders, tabs and splits, with unit tests
- [ ] First successful Chromium build with Compass (needs a contributor machine)
- [ ] WebUI sidebar, window sync, spaces, PiP, …: see the roadmap in ARCHITECTURE.md §13

## License

BSD 3-Clause, the same as Chromium. See [LICENSE](LICENSE).
