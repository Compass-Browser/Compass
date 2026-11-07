# Contributing to Compass

Thanks for helping. Bug reports, ideas and pull requests are all welcome.

## Reporting bugs and asking for features

Open an issue with:

- what you did, what you expected and what happened;
- your platform and Compass version (Settings → About Compass);
- screenshots or a screen recording for anything visual.

Security problems are the exception: report them privately, as described in
[SECURITY.md](SECURITY.md).

## Before you write code

For anything bigger than a small fix, open an issue first so we can agree on
the approach. Compass is a Chromium fork, and the cost of a change is mostly
how it lives with Chromium's monthly updates, so the shape matters more than
usual.

Read [ARCHITECTURE.md](ARCHITECTURE.md), then the design document for the
area you're touching in [docs/design/](docs/design). If your change alters a
design, update its document in the same pull request.

## Setting up

Follow [docs/building.md](docs/building.md). The short version:

```sh
tools/compass init && tools/compass gen debug && tools/compass build
tools/compass run
```

You don't need a Chromium checkout to work on:

- the sync server: `server/` is a Go module ([server/README.md](server/README.md));
- the iOS app: `tools/compass ios` generates and builds the Xcode project;
- the `compass` CLI and build tooling: `python3 -m unittest discover -s tools/tests`;
- the website: plain HTML in `website/`.

## How Compass changes Chromium

In order of preference:

1. **Code in `//compass`** (`browser/`, `components/`, `resources/`, ...),
   wired in through Chromium's existing extension points.
2. **A `chromium_src` override**: a file that replaces or wraps an upstream
   file at compile time ([chromium_src/README.md](chromium_src/README.md)).
3. **A patch** to an upstream file, one per file, kept as small as possible:
   ideally a hook into `//compass` and nothing else
   ([patches/README.md](patches/README.md)). Regenerate patches with
   `tools/compass patches export` and commit them together with the code that
   needs them.

## Code style

- C++, Objective-C++ and GN follow Chromium's style; run `git cl format` from
  the Chromium checkout, or `clang-format` with Chromium's config.
- TypeScript and WebUI code follow Chromium's WebUI conventions.
- Swift: match the code around it.
- Go (`server/`): `gofmt`.
- Python: the style of `tools/compass_tool`, two-space indents.

Comments say why, not what. Names and user-facing strings are plain English.

## Tests

Add or update tests with your change:

```sh
tools/compass test                                  # compass_unittests
tools/compass test --browser                        # compass_browsertests
tools/compass test -- --gtest_filter='SpaceModel*'  # a subset
python3 -m unittest discover -s tools/tests         # tooling
(cd server && go test ./...)                        # sync server
tools/compass ios test                              # iOS (CompassKit)
```

Sync changes that cross devices can also be checked end to end with the
helpers in [tools/sync_e2e](tools/sync_e2e/README.md).

GitHub Actions runs the tooling tests (macOS, Windows, Linux) and the sync
server's tests on every pull request. Full Chromium builds run on
maintainers' machines, so mention what you built and tested.

## Pull requests

- One topic per pull request; keep unrelated refactors out.
- Commit messages: a short summary line naming the area
  ("Sidebar: ...", "Sync: ...", "iOS: ..."), then a body saying what
  changed and why.
- Describe how you tested it, with screenshots for UI changes.
- By contributing, you agree that your contribution is licensed under the
  project's licenses: BSD 3-Clause ([LICENSE](LICENSE)), or MPL-2.0 for
  files in `server/`.
