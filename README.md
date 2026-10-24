# Strata

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
tools/strata init            # depot_tools + Chromium at config/chromium_version (~1 h, ~60 GB)
tools/strata gen debug       # out/Debug with Strata's GN args
tools/strata build           # build Strata (first build: hours)
tools/strata run             # launch with a dev profile (--fresh to reset it)
tools/strata test            # build and run strata_unittests
```

The Chromium checkout goes in `../strata-chromium` next to this repository.
Set `STRATA_CHROMIUM_DIR` to put it somewhere else.

## Repository layout

| Path | What |
|---|---|
| `browser/` | Strata's C++ browser code (`//strata/browser`) |
| `chromium_src/` | Compile-time overrides of upstream files |
| `patches/` | Minimal patches to upstream files, one per file |
| `config/` | Pinned Chromium version and GN args |
| `build/` | Build integration (`redirect_cc.py`) |
| `tools/` | The `strata` CLI and its tests |
| `docs/` | Contributor docs |

## Status

Done in the scaffold:

- [x] `strata` CLI: init, sync, patches, gen, build, run, test, update-chromium
- [x] `chromium_src` override mechanism (compiler wrapper), with tests
- [x] Patches: branding, wiring `//strata/browser` into Chrome, `strata_unittests` in `gn_all`
- [x] `SpaceModel`: sidebar tree with folders, tabs and splits, with unit tests
- [ ] First successful Chromium build with Strata (needs a contributor machine)
- [ ] WebUI sidebar, window sync, spaces, PiP, …: see the roadmap in ARCHITECTURE.md §13

## License

BSD 3-Clause, the same as Chromium. See [LICENSE](LICENSE).
