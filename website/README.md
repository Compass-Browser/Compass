# compassbrowser.org

The homepage: one static page whose download links come from the update
server's `stable.json` and `beta.json` (docs/design/updates.md), and the
privacy policy (also linked from the iOS app's Settings).

It's served from `/var/www/compassbrowser.org` on the server, by the host's
Caddy (`compassbrowser.caddy` here is the site file, also for
updates.compassbrowser.org and sync.example.org). To publish:

```sh
rsync -a --chmod=D755,F644 -e 'ssh -p 22' \
  website/index.html website/privacy.html website/favicon.ico website/compass.svg website/compass-256.png \
  user@server:/var/www/compassbrowser.org/
```
