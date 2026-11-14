# common/reader

Compass's Reader mode, shared by desktop (Mac, Windows, Linux) and iOS. See
[docs/design/reader-mode.md](../../docs/design/reader-mode.md).

| File | What |
|---|---|
| `Readability.js`, `Readability-readerable.js` | Mozilla's Readability, vendored unchanged |
| `LICENSE` | Readability's license (Apache-2.0) |
| `reader.js` | `__compassReader.check()`, `parse()` and `sanitize()`: runs in the page, in a world its scripts can't reach |
| `reader.html`, `reader.css`, `reader_view.js` | The reader view: the template, its look and its script (settings, read aloud) |
| `test/` | `reader_test.html` (with `reader_test.js`) checks `reader.js` on the saved pages in `test/fixtures/` |

## Readability

- Version: **0.6.0** (released 2025-03-03), the npm package
  `@mozilla/readability@0.6.0`
  (`https://registry.npmjs.org/@mozilla/readability/-/readability-0.6.0.tgz`,
  SHA-256 `6d3c3cc50dad51b543925deadc66d5b58cd2262d1d585f2ae0a44975627ad919`),
  the same files as the `0.6.0` tag of `https://github.com/mozilla/readability`.
- `Readability.js` and `Readability-readerable.js` are copied as they are;
  `LICENSE` is the package's `LICENSE.md`.
- To update: replace the two files and `LICENSE` from a newer release, update
  this section, and run the tests below.

## Using it

In the page, in an isolated world (desktop) or a `WKContentWorld` (iOS):

1. To decide whether to offer Reader: inject `Readability-readerable.js` and
   `reader.js`, then call `__compassReader.check()`, which returns
   `{ok, readerable}`.
2. To read: inject `Readability.js`, `Readability-readerable.js` and
   `reader.js` (each defines its globals once per world), then call
   `__compassReader.parse({})`, which returns `{ok, title, byline, siteName,
   excerpt, publishedTime, lang, dir, url, content, length}` or
   `{ok: false, error}`. `content` is sanitized HTML; `length` is the
   article's text length.

Then show `reader.html` (with `reader.css`, `reader.js` and `reader_view.js`
next to it) and give it the article: either as `?article=<URL of the JSON>`
(desktop) or by calling `compassReaderView.render(article)` (iOS). Defining
`compassReaderHost` before `reader_view.js` loads swaps where settings are
kept and what the close button does; see the top of `reader_view.js`.

## Tests

`test/reader_test.html` fetches its fixtures, so it needs an HTTP server:

    python3 -m http.server -d common/reader 8000
    # open http://localhost:8000/test/reader_test.html

Its title becomes `PASS` or `FAIL`, and the page lists failed checks.
`ReaderModeBrowserTest.SharedTests` runs it in Compass.
