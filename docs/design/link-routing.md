# Link routing

Status: done on macOS, Windows and Linux (desktop), on iPhone and iPad
(see "iOS") and on Android (android.md, "Links routed to spaces").

## Goal

Arc's Air Traffic Control: links from other apps on a site you name open in
a space of yours instead of a little window ([little-window.md](little-window.md)).
GitHub links from Slack go to Work, YouTube links from Mail to Home.

## Rules

- A rule names a **domain** and a **space**: `github.com` → Work.
- A rule for a domain covers its **subdomains**: `github.com` takes
  `gist.github.com` and `api.eu.github.com`. An explicit subdomain rule
  wins over its parent's: with `gist.github.com` → Notes as well, gists go to
  Notes and the rest of GitHub to Work. The most specific rule wins.
- Matching looks at the link's host only: the scheme (http or https), port,
  path, query and the host's case don't matter. Only web pages (http and
  https) on a domain are routed; IP addresses, files and other schemes open
  as before.
- **Domains are normalized** when a rule is made (`NormalizeLinkRuleDomain()`,
  `browser/sidebar/link_rules.h`): lowercase, IDN as punycode (`bücher.de`
  is `xn--bcher-kva.de`, which is also how a link's host reads), no leading
  `www.` or `*.`, no trailing dot, port or path. Typing a URL works
  (`https://www.github.com/compass` gives `github.com`). Refused: empty or
  invalid text, IP addresses, other schemes, single names (`localhost`) and
  bare public suffixes (`com`, `co.uk`), which would take a whole top-level
  domain. Unknown suffixes (`router.internal`) and private registries
  (`github.io`) are fine.
- One rule per domain. Rules belong to the **profile** and outlive their
  spaces: a rule whose space was deleted is ignored (the link opens in a
  little window) and settings list it as broken until it's edited or
  deleted.

## What a link from another app does

Only links **from other apps** are routed: the default-browser path little
windows use (`compass::OpenInLittleWindows()`: `application:openURLs:` on
macOS, the command line and the process singleton on Windows and Linux).
Links clicked inside Compass, including ones that open new tabs or windows,
stay where they open, as in Arc.

- A link whose domain matches a rule whose space exists opens as a **new
  Today tab of that space**, active, in:
  1. the last active window showing that space, if there is one;
  2. else the frontmost normal window, switched to that space;
  3. else a new window, showing that space.

  Then the window is activated (Compass comes to the front).
- Any other link opens in a little window, as before.
- **Never incognito.** Off the record (an incognito-only launch, a forced
  incognito profile) nothing is routed: rules are the original profile's,
  and an incognito window is never a target (the windows are looked up in
  the profile itself, `FindLastActiveWindow()`).
- At a cold start the rules come with the spaces' file; links that arrive
  before it has loaded wait for it (`SpaceService::RunWhenLoaded()`).
- `compass::OpenLinkFromAnotherApp()` decides per link;
  `compass::OpenLinkInSpace()` finds the window (`GetWindowForSpace()`,
  shared with the little window's "Open in <space>") and navigates a new
  foreground tab there, which the window's `TabStripBridge` makes a Today
  entry as any new tab.

## Making rules

- **Settings**: compass://settings/links, **Links from Other Apps**
  ([settings.md](settings.md)). The rules by domain, each with its space's
  icon on its color and name (a broken rule says "Space deleted"), **Edit**
  and **Delete**; **Add Site** opens a form: the domain (or a URL) and a
  space. A domain that isn't one gets a note under the form.
- **The little window**: its space menu (⌄ beside "Open in <space>") has
  **Always Open <domain> in ›** with the profile's spaces, the space the
  domain's rule names checked. Picking one makes (or changes) the rule and
  moves the page into that space, as picking the space above does. The ⌄
  button now also shows with one space when the page has a domain, so the
  rule can be made there too.
- **The command bar**: **Route this site to space…** (symbol `alt_route`),
  for a tab on a page with a domain, not in incognito: opens
  compass://settings/links?add=<domain>, the form filled in with the
  current tab's domain and the space of the domain's rule if it has one.

## Storage

`SpaceList` (`browser/sidebar/space_list.h`) holds the rules
(`LinkRules`, `link_rules.h`), next to the spaces and Favorites, so they're
saved in `Compass Sidebar.json`:

```json
"link_rules": [{"domain": "github.com", "space": "<space id>"}]
```

Missing in older files (no rules). An unreadable rule (no space, a domain
that doesn't normalize to itself) is left out. `SpaceList::Observer::
OnLinkRulesChanged()` tells the store to save and the sync bridge to
commit.

## Sync

A new entity in the spaces type, `CompassLinkRule link_rule = 6` in
`CompassSpacesSpecifics`'s oneof
(`components/sync/protocol/compass_spaces_specifics.proto`):

| Field | |
|---|---|
| `uuid = 1` | From the domain, as a favicon's from its host: the first 16 bytes of SHA-256(`"compass-link-rule:" + domain`), byte 6 = `(b & 0x0f) \| 0x50`, byte 8 = `(b & 0x3f) \| 0x80`, lowercase 8-4-4-4-12. Also the client tag. |
| `domain = 2` | Normalized, as above. |
| `space_uuid = 3` | The space's ID (`CompassSpace.uuid`). |

- One entity per domain, keyed by it, so two devices that make a rule for
  the same domain write the same entity; the later commit wins (also for
  a change of space). A delete removes the rule everywhere.
- In the account of the rule's profile, like its spaces.
- A rule whose space is missing on a device (deleted, not synced yet) is
  kept there and routes nothing until the space comes.
- A rule for a space that stays on this device ("Spaces that don't sync")
  isn't uploaded, like the space; turning the space's sync off deletes the
  rule on the other devices.
- **Backward compatible**: older clients ignore the entity (their
  `IsEntityDataValid()` refuses an entity with none of the fields they
  know; the iOS bridge skips specifics it doesn't know). An entity without
  `space_uuid` (a later version's form) is ignored here and never deleted
  by this device; setting the domain's rule here fills it in and keeps
  what this version doesn't know.
- Desktop: `CompassSpacesSyncBridge::AddLocalLinkRules()` in the pass, the
  remote ones applied through `SpaceList::SetLinkRule()`/
  `RemoveLinkRule()` while applying (so nothing echoes), validated in
  `IsEntityDataValid()` (the domain normalizes to itself and the key
  matches). Android shares the bridge and the file, so it keeps the rules
  (no routing there yet).
- iOS: `SyncProto` has the message, and `CompassSpacesBridge` applies the
  rules (keys as `LinkRuleKey()`, `compass_spaces_sync_bridge.h`).

## iOS

iPhone and iPad route too: see [ios.md](ios.md), "Links routed to
spaces". Default-browser links and the share sheet's little-window action
are routed. Where it differs from the desktop: a small built-in
public-suffix list instead of Chromium's, and matching across every
profile's rules (the shown space's profile wins, else the home profile).

## Tests

- `LinkRulesTest.*` (compass_unittests): normalizing (case, `www.`, `*.`,
  ports, paths, schemes, IDN), refusing (empty, spaces, IPs, other schemes,
  single names, public suffixes, wildcards in the middle), matching
  (subdomains, the most specific first, look-alike domains, case, ports and
  paths, IDN, only web pages), `SpaceList`'s routing to existing spaces and
  its observers.
- `SidebarSerializerTest.LinkRulesRoundTrip`, `LinkRulesMissingOrUnreadable`.
- `CompassSpacesSyncBridgeTest.LinkRulesSync`, `LinkRuleOutlivesItsSpace`,
  `LinkRuleOfLocalSpaceStaysHere`, `LinkRuleEntities` (keys, validity, an
  entity missing `space_uuid`, a chain without rules).
- `CommandBarPageHandlerTest.EmptyNewEntryQueryShowsRecentEntries`,
  `CommandsHaveSymbols` (the command and its symbol).
- `LinkRoutingBrowserTest.*` (compass_browsertests), through the same
  entry point as `LittleWindowBrowserTest.LinkFromAnotherApp`
  (`OpenLinksFromAnotherApp()`: the process singleton on Windows and Linux,
  `application:openURLs:` on macOS): `MatchingLinkOpensInTheSpace`,
  `WindowShowingTheSpaceTakesIt`, `OtherLinkOpensALittleWindow`,
  `RuleToDeletedSpaceFallsBack`, `NeverIncognito`; `SettingsSectionsBrowserTest.LinksSection`
  (the form from `?add=`, a broken rule, Delete) and `MenuOpensEachSection`.

## Not done

- Rules by path or by the app the link came from (Arc routes by app too).
- Routing to another profile's space on the desktop: rules name the
  profile's own spaces.
- The settings page doesn't follow changes made while it's shown (it
  refreshes when it's shown again).
