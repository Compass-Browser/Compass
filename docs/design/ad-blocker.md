# Ad blocker

Status: done (desktop and Android). Based on Chromium 157.0.8103.51.

## Goal

A built-in ad blocker on desktop and Android, as on iOS (`ios/App/Model/
ContentBlocking.swift`):

- The same filter lists: EasyList, EasyPrivacy and the EasyList Cookie List,
  all on by default and checked for updates once a day.
- Lists the user adds by URL, each of them with its own switch and a way to
  remove it.
- One switch for the whole ad blocker (on by default).
- A switch for each site (on everywhere by default).

Unlike iOS, which has one "Block Ads" switch for EasyList and EasyPrivacy
together and a separate one for the cookie list, every list has its own
switch. Extensions such as uBlock Origin still run alongside it. Each blocks
on its own, and either can be turned off.

## Upstream does most of it

Chromium's subresource filter (`components/subresource_filter`) is a
filter-list engine already. Upstream feeds it Google's "abusive ads" ruleset
through Safe Browsing. It runs on every page in dry run: it tags ad frames
and blocks nothing. Compass gives it a ruleset built from the user's lists
and turns blocking on for the pages the ad blocker covers.

| Upstream | Compass | Where |
|---|---|---|
| The ruleset service indexes Google's ruleset (`kSafeBrowsingRulesetConfig`, "Subresource Filter" folder) | It indexes the ad blocker's (`compass::adblock::kRulesetConfig`, "Compass Ad Block" folder, its own version prefs) | `browser_process_impl.cc` patch |
| The component updater downloads Google's ruleset | Not registered: it would replace the ad blocker's in the same service | `component_updater-registration.cc` patch |
| Safe Browsing decides each page's activation (`SafeBrowsingPageActivationThrottle`) | The ad blocker decides (`PageActivationThrottle`). The helper gets no database manager, so the Safe Browsing throttle isn't added | `chrome_content_subresource_filter_web_contents_helper_factory.cc` patch; `chrome_content_browser_client_navigation_throttles.cc` patch |
| A page with blocked ads shows "Ads blocked" (Android message, desktop omnibox icon) and records site data (`ADS_DATA`) | Neither: blocking happens on every page | `profile_interaction_manager.cc` patch |
| The ruleset's element hiding rules are indexed, but nothing applies them yet | `CosmeticFilterAgent` injects them as a stylesheet | `subresource_filter_agent.h` patch (an accessor), `chrome_content_renderer_client.cc` patch |
| `ContentSettingsType::ADS`: Block by default, Allow for sites whose ads Google allows | The ad blocker's switch for a site: Block (the default) blocks, Allow turns the blocker off there | Desktop: the Privacy pane's site lists and page info. Android: the Site sheet and Site Settings |

Upstream keeps doing:

- the matching itself (`url_pattern_index`), in each renderer, including
  dedicated workers and child frames;
- `@@...$document` and `$genericblock` exceptions;
- activation inherited by child frames, including out-of-process ones;
- ad tagging (heavy-ad intervention, ad metrics) on the same ruleset.

## Lists

`browser/adblock/filter_lists.{h,cc}`:

- **Built-in lists** (same URLs as iOS's `FilterLists.swift`):
  - `easylist`: `https://easylist.to/easylist/easylist.txt`
  - `easyprivacy`: `https://easylist.to/easylist/easyprivacy.txt`
  - `cookies`: `https://secure.fanboy.co.nz/fanboy-cookiemonster.txt`
- **The user's lists:** https only, no duplicates. Each one's ID is
  `custom-` plus a hash of its URL. Its title is the host and file name until
  the list's "! Title:" header arrives.
- **Local State prefs**, shared by every profile because the ruleset is one
  for the browser (`adblock_prefs.h`):
  - `compass.adblock.enabled` (the switch)
  - `compass.adblock.custom_lists`
  - `compass.adblock.disabled_lists`
  - `compass.adblock.list_state` (ETag, Last-Modified, checked, updated, the
    last error, and rule counts)
- **Downloads** (`FilterListUpdater`), as iOS does them:
  - A list is asked for when its last check is more than a day old: 30 s
    after launch, then every hour. Also when the user adds or turns on a
    list, and on Update Now.
  - Requests send If-None-Match and If-Modified-Since. They carry no cookies.
  - Lists over 30 MB aren't downloaded.
  - A download counts only if it looks like a filter list (`[Adblock` or a
    leading `!`). A failed or bad download keeps the last good copy and
    records the error.
  - Copies live at `<user data>/Compass Ad Block/Lists/<id>.txt`.
  - Nothing downloads while the ad blocker is off.

## Rules

`RulesetBuilder` (`ruleset_builder.cc`) runs on a background sequence, 2 s
after a change (lists, switches or new text):

1. **Each line** goes through `PreprocessLine()` (`abp_preprocessor.cc`):
   - **Skipped**, as iOS skips them: scriptlets (`##+js`), procedural
     selectors (`:has-text()`, `:-abp-*`, `:upward()`, ...), `#?#`, `#$#`,
     `#%#`, HTML filters (`##^`, `$$`), regular expressions, `$redirect`,
     `$csp`, `$removeparam` and other options upstream's parser doesn't know,
     and entity domains (`google.*`).
   - **Rewritten:** uBO's aliases (`3p`, `1p`, `xhr`, `frame`, `css`, `doc`,
     `from=`, `ehide`, `ghide`) become their ABP names, and `$important` and
     `$all` are dropped.
   - **Split:** a hiding rule with excluded domains (`a.com,~b.a.com##.ad`)
     becomes a site rule plus exceptions (`a.com##.ad`, `b.a.com#@#.ad`),
     because upstream's index takes no `~` in hiding rules.
   - **Pop-ups:** `$popup` is dropped, since pop-ups aren't subresources.
2. **Parsing:** upstream's `RuleParser` (`subresource_filter/tools/
   rule_parser`) parses the rule. The builder masks it as upstream's
   converter does (`rule_stream.cc`), and fills each hiding rule's class and
   id anchors (`GetAnchorsIfSupported()`).
3. **Writing:** the ruleset goes out in upstream's unindexed format:
   - Order: network rules, then hiding exceptions and site rules, then the
     rules for every site, as the style index requires.
   - A rule two lists share is written once.
   - Per-list counts (rules used, rules skipped) go to the list's state.
4. **Version:** the content version is a hash of the builder version and
   each enabled list's ID and text hash. The same lists give the same
   version, so a restart doesn't index again; indexing the same version
   twice also fails on Windows. With no list on, the ruleset is empty, so
   whatever was published before stops blocking.

At every launch the service supplies the current ruleset. Upstream's
`RulesetService` skips it if that version is already indexed in the current
format, and indexes it again after a Chromium update changes the format.

Measured on the three default lists (November 2026, debug build):

| | EasyList | EasyPrivacy | Cookie List |
|---|---|---|---|
| Network rules | 50,478 | 56,706 | 2,108 |
| Element hiding rules | 23,565 | 2 | 23,014 |
| Not supported | 4,994 | 82 | 282 |

- All 109,241 network rules and 46,577 hiding rules index. That is 18,189
  explicit selectors, against upstream's cap of 65,534.
- Building takes about 1 s and indexing about 1 s.
- The indexed ruleset is 10 MB.

## Turning it on

`PageActivationThrottle` (`page_activation_throttle.cc`) runs on every
navigation of a subresource filter root (a main frame or a fenced frame),
at request start and after each redirect:

- **Blocking** (the switch is on, the page is http(s), and the profile's Ads
  setting for the site isn't Allow): it notifies the page's activation as
  *enabled* through `SubresourceFilterObserverManager`. Upstream's
  activation-computing throttle then works out the document's exceptions
  and sends the activation to the renderer.
- **Otherwise:** nothing, and the page stays in upstream's dry run. A
  redirect from a site it blocks on to one it doesn't notifies dry run again.

## Element hiding

`CosmeticFilterAgent` (`renderer/adblock/`) is a `RenderFrameObserver`
created next to upstream's `SubresourceFilterAgent`. At
`DidCreateDocumentElement` it reads the frame's document filter. If the
filter blocks (enabled, not dry run, no `@@$document`), it inserts two user
stylesheets of `display:none !important` rules:

- **Rules for every site:** the style index's class and id maps (an implicit
  entry is the class or id itself, an explicit one an index into its
  selectors). The sheet is built once per ruleset in each renderer process
  and shared by frames whose site has no exception. A site with exceptions
  (`#@#`) gets its own copy without them.
- **The site's own rules:** upstream's `GetDomainSelectors()`.

The rules are grouped 20 selectors per rule, so a selector the page's CSS
parser rejects drops only its group. User-origin `!important` rules beat
the page's own `!important`.

Upstream's parser drops rules for every site that have no class or id to
look them up by (`##a[href*="..."]`), and only those. iOS applies them.

## Settings

- **Desktop** (Settings › Privacy):
  - The Ad blocker group: "Block ads and trackers" (the pref, through
    `kAllowedPrefs` as Local State) and Filter lists.
  - The lists view: each list with its switch, rule count, last update and
    error. The user's lists can be removed. There's a field to add a list by
    address, and Update Now.
  - The handler is `settings_window_handler_adblock.cc` (mojom
    `GetFilterLists`, `SetFilterListEnabled`, `AddFilterList`,
    `RemoveFilterList`, `UpdateFilterListsNow`, `FilterListsChanged`).
  - Search finds both rows.
  - The site lists show the Ads setting as "Ads".
- **Desktop, per site:** page info's "Ad blocker" row
  (`browser/ui/views/adblock/adblock_page_info.cc`, next to development
  mode's). Turning it off sets the site's Ads setting to Allow and reloads
  the page; turning it on resets it.
- **Android:**
  - Settings › Privacy › Ad Blocker (`CompassSettingsUi.kt`
    `AdBlockerPage`) has the same controls as the desktop, through
    `AdBlockBridge` (`browser/android/adblock_bridge.cc`).
  - The Site sheet's "Ad Blocker" switch, while the blocker is on, reloads
    the page.
  - Site Settings lists Ads among the defaults.

Setting Ads to Allow as a profile's default turns blocking off for that
profile.

## Not supported

- Scriptlets, `$redirect`, `$removeparam`, `$csp`, regular expressions,
  procedural selectors, `$elemhide` and `$generichide` (upstream's renderer
  filter only checks `$document` and `$genericblock`), and `$popup`.
- Requests a service worker makes, and browser-initiated requests: upstream's
  filter doesn't see them.
- Google's abusive-ads enforcement through Safe Browsing. Compass is built
  without Google API keys, so it didn't get it anyway.

## Tests

- **Unit tests** (`compass_unittests`):
  - `AbpPreprocessorTest`: what's skipped and rewritten, and the splits.
  - `RulesetBuilderTest`: builds, then indexes with upstream's indexer and
    matches with `IndexedRulesetMatcher`.
  - `FilterListUpdaterTest`: the daily gate, 304, errors that keep the last
    copy, custom titles.
  - `AdBlockServiceTest`
  - `CosmeticCssTest`
- **Browser tests** (`compass_browsertests`, `AdBlockBrowserTest`): blocked
  scripts and fetches, generic and site hiding over the page's own
  `!important`, exceptions, the per-site switch, the switch, a cross-site
  iframe, and no "ads blocked" site data.
