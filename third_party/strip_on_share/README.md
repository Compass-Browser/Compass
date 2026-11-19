# Firefox's strip-on-share lists

The query parameters Firefox's "Copy Clean Link" removes, used by Compass's
Copy URL (`compass/common/clean_url.h`). Copied unmodified from Firefox's
`toolkit/components/antitracking/StripOnShareLists/`:

- `MPL2/StripOnShare.json`, under the Mozilla Public License 2.0
  (`MPL2/LICENSE`);
- `LGPL/StripOnShareLGPL.json`, from ClearURLs' rules, under the GNU Lesser
  General Public License 3.0 (`LGPL/LICENSE`).

Source: https://github.com/mozilla-firefox/firefox, commit
`ba5ea450d784ff949c86d9ea91b647605c607c05` (2026-11-17).

Format, for each rule: `queryParams` (names, compared without case),
`hosts` (exact hosts), `schemelessSites` (registrable domains), or
`isGlobal: true` for the rule every site gets. Firefox combines rules of the
same name across both files.

To update: `tools/update_strip_on_share.py` (a weekly workflow,
`.github/workflows/strip-on-share.yml`, runs it and opens a pull request
when the lists changed).
