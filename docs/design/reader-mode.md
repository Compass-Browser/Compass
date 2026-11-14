# Reader mode

Status: shared package done (`common/reader/`); desktop (Mac, Windows,
Linux) in progress; iOS next, on the same package. Based on Chromium
157.0.8103.51.

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
state, its identity in the sidebar and in sync. On top of the content area,
a frameless child widget (as Peek's card, docs/design/peek.md) holds a
`views::WebView` with a second `WebContents`, of the tab's profile, showing
`chrome-untrusted://compass-reader/`.

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

- `ReaderMode` puts the article into `ReaderArticleStore` under a random
  128-bit token and loads `chrome-untrusted://compass-reader/?article=
  article/<token>.json`; `reader_view.js` fetches that. The store keeps
  one article per reader view, of its profile, dropped when the view
  closes.
- **Content Security Policy**: `default-src 'none'`; `script-src 'self'`
  (only `reader.js` and `reader_view.js`); `style-src 'self'`;
  `img-src https: http: data:` (the article's images; http images are
  upgraded as mixed content); `connect-src 'self'` (the article's JSON);
  `font-src 'self'`; `frame-src 'none'`; `form-action 'none'`;
  `frame-ancestors 'none'`. Even a script that got past the sanitizer would
  not run.
- `<meta name="referrer" content="no-referrer">`: the article's images
  load without saying where from.

### Showing and leaving

`compass::ReaderMode` (`browser/ui/views/reader/`), one per normal window,
owned by the sidebar view next to Peek. Each tab's Reader state is
`ReaderTabState`, user data on its `WebContents`:

- **Is this an article?** When the active tab finishes loading
  (`DocumentOnLoadCompletedInPrimaryMainFrame`), and 1 s after a
  same-document navigation (single-page apps), `ReaderMode` runs
  `Readability-readerable.js` and `reader.js` `check()` in the page's
  isolated world (`ISOLATED_WORLD_ID_CHROME_INTERNAL`, as Claude's page
  agent, docs/design/claude.md). Only http(s) and file pages, only the
  active tab: background tabs are checked when they come forward.
- **Entering**: `parse()` the same way, with Readability, then the reader
  view over the content area, focused. A page that has no article still
  enters: the view says "This page has no article to show." (the shortcut
  works on any page, as in Safari).
- **Leaving**: Esc (after the reader page, which uses it to close its
  settings panel first), the Reader button or the shortcut again, the
  view's × button (`window.close()`, which reaches `CloseContents`), or
  anything that commits a new document in the tab (a link, Back, a reload,
  typing a URL). The page underneath gets the focus back.
- **Switching tabs** hides the view; coming back to a tab that was in Reader
  shows it again with the article it had. Reader is per tab; one reader view
  per window shows the active tab's.
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

- Little windows and other windows without a sidebar have no Reader.
- Find on page (Cmd+F) searches the live page under the view, not the
  article. Printing prints the live page.
- Pages that need a sign-in cookie for their images show them only if the
  image host doesn't check the referrer.

### Tests

`ReaderModeBrowserTest` (`browser/ui/views/reader/reader_mode_browsertest.cc`),
with the fixtures served by the embedded test server:

- `SharedTests`: `reader_test.html` passes in Compass.
- `ArticleShowsReader`: the article fixture loads, the pill's Reader button
  appears; the toggle shows the reader view with the title and text, and
  the tab's URL and history length haven't changed; the page underneath
  still has its scripts' state; Esc leaves, the view goes, focus is back on
  the page.
- `NotAnArticle`: the sign-in fixture shows no Reader button.
- `NavigationLeaves`, `TabSwitchKeepsReader`, `CommandAndShortcut`: a new
  document leaves Reader; switching away hides and back shows it; the
  command bar's command and the shortcut toggle it.
- `Incognito`: Reader works in an Incognito window.
