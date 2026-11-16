# compassbrowser.org

The homepage: one static page whose download links come from the update
server's `stable.json` and `beta.json` (not `canary.json`;
docs/design/updates.md), and Linux's from GitHub Releases (Linux isn't in the
feeds yet; canary builds are left out), the privacy policy (also linked from the iOS
app's Settings), and a changelog for each platform in `changelog/`
(`/changelog/mac`, `/changelog/windows`, `/changelog/linux`,
`/changelog/ios`), written by hand
for each release, and `/boost` (`boost.html`), which shows a shared Boost from
its link (docs/design/boosts.md, "Sharing") without sending it anywhere.

There's no build step. To preview it, serve this folder locally:

```sh
python3 -m http.server -d website 8000
```

To publish, copy `index.html`, `privacy.html`, `compare.html`, `boost.html`,
`boost.js`, `boost-page.js`, `favicon.ico`, `compass.svg`,
`compass-256.png` and the `changelog` folder to the web server's document root.

`compare.html` is made from `COMPARE.md`: after editing that, run
`python3 tools/make_compare_page.py`.
