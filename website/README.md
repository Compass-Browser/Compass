# compassbrowser.org

The homepage: one static page whose download links come from the update
server's `stable.json` and `beta.json` (not `canary.json`;
docs/design/updates.md), and Linux's from GitHub Releases (Linux isn't in the
feeds; prereleases are left out, so it's the stable Flatpak branch's), the privacy policy (also linked from the iOS
app's Settings), and a changelog for each platform in `changelog/`
(`/changelog/mac`, `/changelog/windows`, `/changelog/linux`,
`/changelog/ios`), written by hand
for each release, and `/boost` (`boost.html`), which shows a shared Boost from
its link (docs/design/boosts.md, "Sharing") without sending it anywhere,
and `/join` (`join.html`), which shows what a sharing invitation is (name,
kind, server; never its secrets) and hands it to Compass (`compass://join#`).
`/oauth/cloudflare` (`oauth/cloudflare.html`) is the redirect URI of
"Sign in with Cloudflare" (docs/design/sync.md, "A free server on
Cloudflare"): Compass catches the navigation to it and closes the tab, so
the page only shows when the sign-in didn't come from Compass, and says
so.
`/terms/cloudflare` (`terms/cloudflare.html`) is the terms for setting up a
sync server on Cloudflare and for using one you're invited to (also the
Cloudflare OAuth client's terms of service URL).

There's no build step. To preview it, serve this folder locally:

```sh
python3 -m http.server -d website 8000
```

To publish, copy `index.html`, `privacy.html`, `compare.html`, `boost.html`,
`boost.js`, `boost-page.js`, `join.html`, `join-page.js`, `site.css`, `favicon.ico`, `compass.svg`,
`compass-256.png` and the `changelog`, `fonts`, `img`, `oauth` and `terms` folders to the
web server's document root.

`site.css` is the shared look (colors, fonts, nav, buttons, footer); every
page links it, the changelog and small pages before `changelog/changelog.css`.
The fonts in `fonts/` (Geist, Geist Mono, Instrument Serif italic) are
self-hosted under the SIL Open Font License, whose text is beside them. A
dark palette is kept in `site.css` for later, off unless the page's `<html>`
has `data-theme="dark"`. The homepage's phones show screenshots in `img/` (600 px wide WebP):
`tools/website/capture_ios.sh` makes `ios-*.webp` from the iOS demo sidebar
in the simulator (`ios/UITests/WebsiteScreenshotsTests.swift`), and
`tools/website/capture_android.sh <name>` makes `android-<name>.webp` from
what the Android emulator shows (set up by hand: Android has no demo
sidebar yet). `tools/website/capture_mac.sh` makes `mac-*.webp` (the
window, and its sidebar alone) from the installed Compass in a throwaway
profile seeded with `tools/website/demo_sidebar.json`, including Work's split
view (clicked over the DevTools protocol, `tools/website/cdp_eval.mjs`) and
the Claude panel (a real answer from your own Claude Code, in Claude Code
mode). The command bar feature is still drawn in CSS (`.palette`).

`compare.html` is made from `COMPARE.md`: after editing that, run
`python3 tools/make_compare_page.py`.
