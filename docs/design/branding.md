# Branding

Status: strings and `compass://` done. Based on Chromium
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

The user types and sees `compass://`; pages still load as `chrome://`, so
every WebUI config, `chrome://` check and content security policy upstream
(and ours: `chrome://compass-sidebar` and the others) stays as it is.
`compass://compass-sidebar` reaches the sidebar page like any other.

### In

- `compass` is a standard scheme in every process
  (`ChromeContentClient::AddAdditionalSchemes()` patch), so `compass://x/y`
  parses with a host and path.
- `compass::AddCompassURLHandler()` (`browser/compass_url_handler.cc`) is
  the first `BrowserURLHandler` pair (patch in
  `ChromeContentBrowserClient::BrowserURLHandlerCreated()`). It turns
  `compass://x` into `chrome://x` and runs the rest of the handlers on it
  itself, then returns true: the first handler that returns true ends the
  chain, so returning false would keep upstream's rewrites (new tab page,
  `chrome://about`, `chrome://help`) but leave the entry's virtual URL as
  `compass://` for good. Its reverse handler keeps the committed URL, so
  once the page commits the tab's visible URL is `chrome://x`, which is what
  upstream code checks (new tab page, WebUI, sessions). While it loads, the
  visible URL is the typed `compass://x` (`IsNewTabPage()` and the command
  bar map it back with `ToChromeURL()`).
- `ProfileIOData::IsHandledProtocol()` lists `compass` (patch): the omnibox
  and command bar take `compass://settings` as a URL, not a search; startup
  and link context menus accept it; it never goes to the OS as an external
  protocol.
- A web page can't reach `chrome://` through it. Since `compass` is a
  handled scheme, content won't let a renderer request it
  (`ChildProcessSecurityPolicy::CanRequestURL()`), so a page's
  `compass://` link or `window.open()` is blocked like one to `chrome://`.
  Test `WebPagesCantOpenCompassURLs`.

### Out

`compass/common/compass_url.h`: `ToCompassURL()` and `ToCompassText()`
write a `chrome://` URL as `compass://` where the user sees or copies it:

- the pill (`SidebarHeader::GetPillText()`), the command bar's starting text
  (Cmd+L), its entry titles and subtitles and its omnibox matches;
- Copy Link in the sidebar's context menu, and upstream's Copy URL
  (Cmd+Shift+C, `chrome::CopyURL()` patch);
- the (hidden) location bar
  (`ChromeLocationBarModelDelegate::FormattedStringWithEquivalentMeaning()`
  patch).

`chrome://` pages aren't saved in history, so nothing to do there. Saved
sidebar entries keep whatever URL the tab had (`chrome://` after commit);
both load.

### Not covered

- `view-source:compass://...` (upstream's view-source handler only takes
  the schemes it knows).
- The omnibox's built-in suggestions (`BuiltinProvider`) list `chrome://`
  pages for "chrome://" input, not for "compass://"; their text shows as
  `compass://` in the command bar.
- Upstream UI that prints a URL some other way (page info, the task
  manager, `chrome://chrome-urls` itself) still says `chrome://`.

### Rebuild cost

Six one-line hooks in `.cc` files plus `deps` in their targets; no header
anyone else includes changes. 18 steps in out/Debug, 21 in out/Fast.

Tests: `compass/common/compass_url_unittest.cc`,
`SidebarHeaderTest.PillText`,
`SidebarHeaderBrowserTest.CompassSchemeOpensChromePages` and
`.WebPagesCantOpenCompassURLs`.
