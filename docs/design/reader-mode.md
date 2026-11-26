# Reader mode

Status: shared package done (`common/reader/`); desktop (Mac, Windows,
Linux) done; iOS done, on the same package ("iOS" below); Android done,
on the desktop's reader page (android.md, "Reader mode"). Based on
Chromium 157.0.8103.51.

## Goal

Safari's Reader: on a page that reads as an article, a Reader button shows
in the address area; pressing it shows the article alone, in clean type,
in the same tab. Esc or the button again goes back to the live page, as it
was.

- **One engine everywhere.** Mozilla's Readability.js decides what the
  article is on every platform, so a page reads the same in Compass on a Mac,
  on Windows, on Linux and on an iPhone.
- **Never sends the page anywhere.** Extraction runs in the page's renderer;
  the reader view is drawn by Compass; nothing goes to a server. It works in
  Incognito.
- **Settings**: font (serif or sans), size, width, theme (follows the
  system's light or dark, or Light, Sepia, Dark), remembered per profile.
- **Read aloud** with the system's own voices, when there are any.

### Not Chromium's Reading mode

Chromium has a Reading mode of its own (`read_anything`): a side panel that
distills the page through the accessibility tree, with Google's Screen2x
model where available. Compass doesn't use it:

- Its extraction differs from what iOS can run; Readability.js runs in any
  web view, so desktop and iOS share one engine, one sanitizer and one look.
- It's a side panel next to the page; Reader in Compass replaces the page,
  as in Safari and Arc's peers.
- Its shortcut and menu item lead to Compass's Reader instead (see
  "Commands").

## The shared package: `common/reader/`

See its [README](../../common/reader/README.md) for the files and how a
platform uses them.

- **Readability.js 0.6.0** and `Readability-readerable.js`, vendored
  unchanged with their Apache-2.0 `LICENSE`; the README names the release,
  its source and checksum.
- **`reader.js`** defines `__compassReader` in the world it runs in:
  - `check()`: `isProbablyReaderable(document)`, `{ok, readerable}`. Cheap:
    it needs only `Readability-readerable.js` (4 kB), not Readability.
  - `parse()`: Readability on a clone of the document (Readability changes
    what it reads; the page stays as it is), then the sanitizer. Returns
    `{ok, title, byline, siteName, excerpt, publishedTime, lang, dir, url,
    content, length}`, the texts trimmed to sane lengths.
  - `sanitize(html, base)`: an allowlist, in an inert document (a
    `<template>`'s content: nothing loads or runs while it's cleaned).
    Text elements, lists, tables, figures, images and links are kept, with
    a few attributes each (`href`, `src`, `srcset`, `alt`, `colspan`,
    `lang`, `dir`, ...); no `class`, `id`, `style` or `on*`. Scripts, styles,
    frames, forms and their controls, SVG, MathML, media players, objects
    and templates go with their content; unknown elements are unwrapped.
    URLs are made absolute against the page and kept only for http(s),
    `mailto:` links and `data:image/...` images; links get
    `rel="noopener noreferrer"`.
- **The reader view**: `reader.html` (the template: a toolbar, the settings
  panel, the article), `reader.css` (its look, the four themes) and
  `reader_view.js` (fills the template, settings, read aloud). Desktop and
  iOS show the same files, so Reader looks the same on both. The view
  sanitizes the article again before it puts it in the page: the article
  comes from a renderer that ran the site's code.
- **Settings** are `{font, size, width, theme}`, kept by
  `reader_view.js` in its origin's `localStorage` unless the host gives it
  `compassReaderHost.loadSettings()`/`saveSettings()` (iOS can use its
  own storage). On desktop that origin is `chrome-untrusted://compass-reader`,
  whose storage is per profile; an Incognito profile's is in memory and
  starts from the defaults.
- **Read aloud** (`Listen` in the toolbar): Web Speech's `speechSynthesis`
  with a voice whose `localService` is true and that matches the article's
  language; the button is hidden when there's none. Remote voices are never
  used, so the text stays on the device.
- **Tests**: `common/reader/test/reader_test.html` (+ `reader_test.js`)
  runs `reader.js` on saved pages (`test/fixtures/article.html`, a news-like
  article with scripts, handlers, an iframe, a form and relative URLs;
  `not_article.html`, a sign-in page) and the sanitizer on a list of
  cases (scripts, handlers, `javascript:` and `data:` URLs, frames, forms,
  SVG, ...). The title turns `PASS` or `FAIL`. Desktop runs it as a
  browser test.

## Desktop

### How the view replaces the page

Reader shows over the tab, not in it. The tab's `WebContents` is left alone:
its URL, its history (back and forward), its scroll position and form
state, its identity in the sidebar and in sync. A second `WebContents`, of
the tab's profile, shows `chrome-untrusted://compass-reader/` in a
`views::WebView` that is the last child of the pane showing the tab (its
`ContentsContainerView`), with the bounds of the pane's page
(`contents_view()`), so it covers the page and nothing else: the devtools
dock and the other panes of a split stay as they are. The pane's layout
manager only lays out the children it knows, so the view keeps the bounds
`ReaderTabState` gives it, following the page's. As the last child it is
drawn over the page on every platform (views orders native views by their
z-order, as for upstream's own overlays in that pane). It's in the window's
own widget: focus, the frame's active look and window moves need nothing
special. (Peek's card, a child widget, was the other way; its window
juggling buys nothing here.)

Alternatives, and why not:

- **Navigating the tab to a reader URL** (`compass://reader?url=...`, as
  Chromium's old DOM distiller did with `chrome-distiller://`): the tab's URL
  changes, a history entry is added (Back would go to the article, not to
  the page before it), and the page reloads when Reader is left.
- **Rewriting the page's own DOM** (hiding its body, inserting the article):
  keeps the URL, but the site's scripts and styles still run against it,
  its Content Security Policy decides what the article may load, and leaving
  Reader depends on the site not having changed the DOM meanwhile.
- **Chromium's Reading mode**: a side panel, see above.

Over the tab, the reader view is Compass's own page: its own Content
Security Policy, its own scripts only, and the live page waiting
underneath, untouched, when Reader ends.

### The reader page

`chrome-untrusted://compass-reader/` (`ReaderUntrustedUI`,
`browser/ui/webui/reader/`) is untrusted WebUI: no Mojo, no `chrome.send`,
no access to the browser beyond what any page has. It serves the shared
files from strings built into the browser (`//compass/browser/reader:assets`,
as `common/claude/` is, `build/embed_text.py`), and the article as JSON:

- `ReaderTabState` puts the article into `ReaderArticleStore` under a random
  128-bit token and loads `chrome-untrusted://compass-reader/?article=
  article/<token>.json`; `reader_view.js` fetches that. The store keeps
  one article per reader view, of its profile, dropped when the view
  closes.
- **Content Security Policy**: `default-src 'none'`; `script-src 'self'`
  (only `reader.js` and `reader_view.js`); `style-src 'self'`;
  `img-src https: http: data:` (the article's images; http images are
  upgraded as mixed content); `connect-src 'self'` (the article's JSON);
  `frame-src 'none'`; `object-src 'none'`; `form-action 'none'`;
  `frame-ancestors 'none'`; Trusted Types off (the view sets sanitized
  HTML with `innerHTML`). Even a script that got past the sanitizer would
  not run.
- `<meta name="referrer" content="no-referrer">`: the article's images
  load without saying where from.

### Showing and leaving

`compass::ReaderMode` (`browser/ui/views/reader/`), one per window with a
sidebar, owned by the sidebar view next to Peek. Each tab's Reader state is
`ReaderTabState`, user data on its `WebContents`, made when the tab first
comes to the front. `ReaderMode` follows the active tab and the panes (a
pane that shows another tab says so, `AddWebContentsAttachedCallback()`),
puts each tab's view over the pane that shows it, and updates the header.

- **Is this an article?** When the page finishes loading
  (`DocumentOnLoadCompletedInPrimaryMainFrame`, and once more 2 s later for
  pages that build their text late), and 1 s after a same-document
  navigation (single-page apps), `ReaderTabState` runs
  `Readability-readerable.js` and `reader.js` `check()` in the page's
  isolated world (`ISOLATED_WORLD_ID_CHROME_INTERNAL`, as Claude's page
  agent, docs/design/claude.md). Only http(s) and file pages; a tab that
  was never in front isn't checked until it is.
- **Entering**: `parse()` the same way, with Readability, then the reader
  view over the tab's pane, focused. On a page that isn't an article the
  command and the shortcut still enter: the view shows what Readability
  found, or says there's no article.
- **Leaving**: Esc (after the reader page, which uses it to close its
  settings panel first), the Reader button or the shortcut again, the
  view's × button (`window.close()`, which reaches `CloseContents`), or
  anything that commits a new document in the tab (a link, Back, a reload,
  typing a URL). The page underneath gets the focus back.
- **Switching tabs** takes the view away (its `WebContents` stays with the
  tab); coming back to a tab that was in Reader shows it again as it was.
  Reader is per tab, so in a split each pane can be in Reader or not.
- **Links in the article**: a plain click loads the link in the tab, which
  leaves Reader (the reader page is WebUI, so Blink sends every link to the
  browser as `OpenURLFromTab`); Cmd/Ctrl+click, middle click and
  `target=_blank` open a tab, as from the page.
- **Window shortcuts** work from the reader view (Cmd+T, Cmd+L, ...), as
  from Peek: keys the view doesn't use go to the window.

### Where Reader shows

- **The pill** (docs/design/command-bar.md, "Slice 2"): a book icon at the
  pill's right, before the content setting icons, while the active tab is
  an article or in Reader; highlighted while in Reader. Its tooltip says
  "Show Reader" or "Hide Reader", with the shortcut. Laid out collapsed the
  header has no pill and no Reader button.
- **The command bar**: "Reader Mode" while the active tab is an article,
  "Leave Reader Mode" while it's in Reader, with the `menu_book` symbol.
- **Commands**: `kToggleReaderModeCommand` (`compass_command_ids.h`).
  Upstream's Reading mode commands (`IDC_SHOW_READING_MODE` from the app
  menu, `IDC_SHOW_READING_MODE_KEYBOARD`, the page context menu's "Open in
  reading mode") run it too (`HandleReaderCommand()`), so Chromium's side
  panel never opens.
- **Shortcut**: Cmd+Option+R on macOS and Shift+Alt+R on Windows and
  Linux, the keys upstream gave its Reading mode (Firefox's Reader View has
  Cmd+Option+R too; Safari's Cmd+Shift+R is Compass's hard reload). Listed in
  compass://settings/shortcuts under Compass as "Reader mode" and
  changeable there (docs/design/shortcuts.md).

### Not in Reader

- Little windows and other windows without a sidebar have no Reader
  (upstream's Reading mode commands do nothing there).
- The page context menu's "Open in reading mode", when upstream shows it,
  still opens Chromium's side panel: context menu commands don't go through
  the browser's command controller.
- Find on page (Cmd+F) searches the live page under the view, not the
  article. Printing prints the live page.
- Pages that need a sign-in cookie for their images show them only if the
  image host doesn't check the referrer.

### Tests

`ReaderModeBrowserTest` (`browser/ui/views/reader/reader_mode_browsertest.cc`),
with the fixtures served by the embedded test server:

- `SharedTests`: `reader_test.html` passes in Compass.
- `ArticleShowsReader`: the article fixture loads, the pill's Reader button
  appears; pressing it shows the reader view over the tab's pane with the
  title and text and none of the scripts, handlers, frames or forms; the
  tab's URL and history length haven't changed and the page still has its
  scripts' state; Esc leaves and the view goes.
- `NotAnArticle`: the sign-in fixture shows no Reader button (and loses it
  coming from an article); the command still enters.
- `NavigationLeaves`, `LinkLoadsInTheTab`, `TabSwitchKeepsReader`,
  `Commands`, `SettingsAreRemembered`: a new document leaves Reader; a link
  in the article loads in the tab; switching away takes the view away and
  back brings it; the Reader command and upstream's Reading mode commands
  toggle it; the theme and font chosen stay for the next article.
- `Incognito`: Reader works in an Incognito window.
- The keys: `ShortcutsTableTest` and `CompassShortcutsBrowserTest`
  (`ArcShortcuts` on Windows and Linux, its macOS twin in
  `shortcuts_mac_browsertest.mm`).

## iOS

The same package in WKWebView (`ios/App/Browser/Reader.swift`); the files
are bundled through `ios/project.yml`, as `common/claude/` is. Nothing goes
anywhere; it works in Incognito.

- **Is this an article?** When a tab's page finishes loading, and 1 s
  after a URL change within the page (single-page apps),
  `Readability-readerable.js` and `reader.js` `check()` run in the page in
  Compass's own content world (`WKContentWorld` "CompassReader"), never the
  page's. The scripts are evaluated as scripts there first (their globals
  once per world), then `check()` or `parse()` is called. Only http(s)
  pages; every tab, not only the shown one (`ReaderMode`, one per app;
  `readerable` by tab and URL).
- **The reader page.** Over the tab, a WKWebView of its own (`ReaderPage`)
  loads the shared `reader.html` from `compass-reader://reader/`, a scheme
  the app serves (`ReaderSchemeHandler`): only `reader.html`, `reader.css`,
  `reader.js` and `reader_view.js`, with the desktop's Content Security
  Policy as a header and as a `<meta>` (`default-src 'none'`, `script-src
  'self'`, `img-src https: http: data:`, ...), and `no-referrer`. The page
  uses the tab's website data store (Incognito's stays in memory). Once it
  has loaded, the app hands it `parse()`'s result
  (`compassReaderView.render()`); `reader_view.js` sanitizes it again, so
  the look, the toolbar (×, Listen, Aa), the settings panel and the reading
  time are the desktop's.
- **`compassReaderHost`**: a user script at document start in the reader
  page gives `reader_view.js` the settings from UserDefaults
  (`compass.reader.settings`, the same JSON the desktop keeps in
  localStorage), saves changes back through a script message, and makes ×
  and Esc (with the settings panel closed) leave Reader. One set of
  settings for the app, not per profile.
- **Listen** is `reader_view.js`'s, with WebKit's `speechSynthesis` and
  the system's local voices: nothing native.
- **Where Reader shows** (ios.md, "Reader mode"): a book at the end of the
  bar's address pill while the shown tab is an article or in Reader
  (filled and tinted in Reader); Show/Hide Reader in the pill's long-press
  menu; a Reader tile in the page menu (dimmed when the page isn't an
  article); and ⌘⌥R with a hardware keyboard (the Mac's keys), which works
  on any web page: one without an article shows "No article found on this
  page."
- **Leaving**: the book or ⌘⌥R again, the reader's ×, Esc, or a new
  document committed in the tab (`TabManager.onCommitted`: a link, Back, a
  reload). Reader is per tab: each window keeps its tabs' readers
  (`AppModel.readers`), showing the shown tab's; switching away and back
  shows it again.
- **Links in the article** load in the tab, which leaves Reader; "Original
  page" just leaves; `mailto:` goes to the system.
- **Unlike the desktop**: Find on Page (⌘F) searches the article while
  Reader is up; splits show the reader over the front pane.
- **Tests**: `ReaderTests` in CompassKit (settings as `reader_view.js`
  validates and stores them, reader.js answers, the served files and their
  policy, links); UI test `ReaderModeTests` on the package's fixtures (the
  book shows on the article and not on the sign-in page; the reader shows
  the article without the page's ads; Aa → Sepia is kept for the next
  reader; × goes back).

## Not yet

- **Localization.** Reader's own labels (`common/reader/reader.html`,
  `reader_view.js`, about 20) are English on every platform. iOS loads the
  files as they are and the desktop serves them through a request filter
  without template replacement, so `$i18n{}` won't do; the strings need
  passing in from each platform.
