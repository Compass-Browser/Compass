# Branding

Status: strings done; `compass://` not started. Based on Chromium
156.0.8078.48.

## Goal

Nothing the user reads says Chromium where it means this browser. The
product's own name comes from `BRANDING` (window titles, the app menu,
About, the bundle). This covers the rest: upstream's strings that name the
product outright, and the `chrome://` scheme (to become `compass://`).

## Strings

Upstream keeps product-named text in Chromium-branded strings files, plus a
few messages in shared ones (in `<if expr="not _google_chrome">` branches).
Six files have any, and grit builds each from a renamed copy:

| File | Messages renamed |
|---|---|
| `chrome/app/chromium_strings.grd` (with `settings_chromium_strings.grdp`) | 676 |
| `components/components_strings.grd` (autofill, omnibox, page info, ...) | 99 |
| `components/components_chromium_strings.grd` | 20 |
| `chrome/app/generated_resources.grd` (password manager, settings) | 12 |
| `extensions/strings/extensions_strings.grd` | 5 |
| `components/privacy_sandbox_strings.grd` | 2 |

The list is `compass_rebranded_grds` in `strings/strings.gni`. To find new
ones after a Chromium update, run `rename_messages()` over every `.grd` (see
the commit that added this file for the scan).

### Renaming

`strings/rebrand_strings.py` copies a `.grd`, its parts and its 81
translations to `gen/compass/strings/<same path>`, changing "Chromium" to
"Compass" in message text only (not descriptions, names or conditions):

- Whole words with ASCII boundaries: "ChromiumOS" stays, "Chromium을"
  (Korean) is renamed.
- The project keeps its name: "made possible by the Chromium open source
  project", "The Chromium Projects", and the link text between
  `BEGIN_LINK_CHROMIUM` and `END_LINK_CHROMIUM`. So About reads "Compass is
  made possible by the [Chromium] open source project".
- "The Chromium Authors" becomes "The Compass Authors" (as `BRANDING`).
- Placeholder examples (`<ex>`) are left alone.

A message's translation ID is a fingerprint of its text, so renamed messages
get new IDs. The script reads both versions with grit's own parser and copies
each renamed message's translations under its new ID, with the same renaming
(translations keep "Chromium" in Latin letters in every language). A message
that names both the product and the project keeps its translations
unchanged: word order differs, so the two can't be told apart.

Known rough edges: Korean particles follow the old name ("Compass은" where
"Compass는" reads right), and any other language that inflects the name keeps
Chromium's form.

### Building

`patches/tools-grit-grit_rule.gni.patch`: the `grit` template, given one of
those sources, builds the copy instead and depends on
`//compass/strings:rebranded_grds`. No upstream `BUILD.gn` changes.

grit looks a `.grd`'s start IDs up by its path in
`tools/gritsettings/resource_ids.spec`. Giving the copies their own entries
there doesn't work: the spec's start IDs are computed from each file's
message count, a generated file needs a hand-written size, and extra entries
upset the spec's `join` counts (everything after `privacy_sandbox_strings`
moved by 20000 when tried). Instead the action copies each original's
computed start from upstream's `default_resource_ids` into
`gen/compass/strings/resource_ids` under the copy's path, and the template
passes that file to grit for these six. Same IDs, so `branded_strings.h`,
`generated_resources.h` and the others stay byte-identical (grit writes
headers only when they change) and nothing recompiles: the first build after
this change was 520 steps, about a minute, in out/Debug.

The action lists its outputs with `exec_script` at `gn gen` time, which
needs `//compass/strings/BUILD.gn` in `.gn`'s `exec_script_allowlist`
(`patches/.gn.patch`).

`InfoPlist.strings` (macOS permission prompts, "Compass Shortcut") is built
from the same paks, so it follows.

Tests: `tools/tests/test_rebrand_strings.py`.

## `compass://`

Not started. Plan: Brave's approach. Register `compass` as a standard scheme
handled by the same WebUI: a `BrowserURLHandler` rewriter maps `compass://`
to `chrome://` on the way in, and a reverse rewrite shows `compass://` in the
pill, the omnibox and history. Our pages (compass-sidebar,
compass-command-bar, compass-placeholder, compass-favicon) would then be
reached as `compass://`.
