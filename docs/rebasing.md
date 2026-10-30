# Updating Chromium

Compass follows Chrome stable. Expect a new major version every 4 weeks and
security respins in between. Security respins should ship within days.

## Steps

```sh
# 1. Find the new stable version.
curl -s 'https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Mac&num=1'

# 2. Move the checkout. This reverts patches, fetches the tag, syncs DEPS,
#    re-applies patches with a 3-way merge, and updates config/chromium_version.
tools/compass update-chromium 157.0.XXXX.YY
```

If patches fail:

```sh
# Conflicts are left in src/ as merge markers. Fix them, then:
tools/compass patches export
tools/compass patches status     # everything should say "applied"
```

Then:

1. Fix any **orphaned overrides** that `update-chromium` reports. An upstream
   file was renamed or deleted, and the override is being ignored.
2. Run `tools/compass gen debug` and fix GN arguments Chromium removed.
3. Run `tools/compass build`, then `tools/compass test`.
4. Do a manual smoke test: sidebar, spaces, window sync, PiP, the top
   extensions.
5. Commit `config/chromium_version`, `patches/` and any code fixes as one
   commit: `Update Chromium to <version>`.

## Keeping updates cheap

- Every patch is a liability. When you touch one during an update, ask
  whether it could be an override or a hook into `//compass` instead.
- Watch upstream features we build on (vertical tab strip region, split tabs,
  auto-PiP). When they change, fix our code rather than patching theirs back.
- Major version updates are best done in a branch with CI building all three
  platforms before merge.
