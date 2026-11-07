# compassbrowser.org

The homepage: one static page whose download links come from the update
server's `stable.json` and `beta.json` (docs/design/updates.md), and the
privacy policy (also linked from the iOS app's Settings).

There's no build step. To preview it, serve this folder locally:

```sh
python3 -m http.server -d website 8000
```

To publish, copy `index.html`, `privacy.html`, `favicon.ico`, `compass.svg`
and `compass-256.png` to the web server's document root.
