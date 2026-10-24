# Patches

Small changes to upstream Chromium files that can't be done in `//strata` or
with a `chromium_src` override. There's one patch per file, named after its
path with `/` replaced by `-`.

| Location | Applies to |
|---|---|
| `patches/*.patch` | `src/` |
| `patches/<repo>/*.patch` | the nested git repository `src/<repo>/` (for example `patches/v8/`) |

## Workflow

```sh
# 1. Edit the upstream file directly in the checkout.
$EDITOR ../strata-chromium/src/chrome/browser/BUILD.gn

# 2. Regenerate patches/ from everything that differs from the pinned tag.
tools/strata patches export            # add --repo v8 for a nested repo

# 3. Commit the patch together with the //strata code that needs it.
```

Other commands: `strata patches status | apply | revert`. Applying is
idempotent, so patches that are already applied are skipped.

## Keep patches tiny

Every line here can conflict on each monthly Chromium update. A good patch
adds a hook (a dependency, a call into `//strata`, a friend declaration) and
nothing else.

## Current patches

| Patch | Why |
|---|---|
| `BUILD.gn.patch` | Adds `//strata:strata_unittests` to `gn_all` |
| `chrome-browser-BUILD.gn.patch` | Links `//strata/browser` into the browser |
| `chrome-app-theme-chromium-BRANDING.patch` | Product name, company and bundle ID |
