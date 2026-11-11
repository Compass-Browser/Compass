# compassbrowser.org

The homepage: one static page whose download links come from the update
server's `stable.json` and `beta.json` (not `canary.json`;
docs/design/updates.md), and the privacy policy (also linked from the iOS
app's Settings), and a changelog for each platform in `changelog/`
(`/changelog/mac`, `/changelog/windows`, `/changelog/ios`), written by hand
for each release.

There's no build step. To preview it, serve this folder locally:

```sh
python3 -m http.server -d website 8000
```

To publish, copy `index.html`, `privacy.html`, `favicon.ico`, `compass.svg`,
`compass-256.png` and the `changelog` folder to the web server's document root.
