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
`boost.js`, `boost-page.js`, `join.html`, `join-page.js`, `favicon.ico`, `compass.svg`,
`compass-256.png` and the `changelog`, `oauth` and `terms` folders to the
web server's document root.

`compare.html` is made from `COMPARE.md`: after editing that, run
`python3 tools/make_compare_page.py`.
