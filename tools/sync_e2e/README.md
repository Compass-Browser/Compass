# Sync end-to-end helpers

Small scripts that drive headless Compass instances through the DevTools
protocol, so two or more of them can sync through a sync server and be
checked from a shell. They are run by hand when sync changes, not on every
build. The design they test is in
[docs/design/sync.md](../../docs/design/sync.md).

Nothing opens a window: every instance runs `--headless` and never takes
focus. The helpers need macOS (the instances are `Compass.app`), Python 3
(standard library only) and bash. Run them from the repository root in
bash, not zsh.

## The helpers

Running instances:

| Helper | What it does |
|---|---|
| `instance.sh start NAME PORT [switches...]` / `stop NAME` | Starts or stops one headless instance with its own user data dir under `$E2E_DIR/NAME` and a DevTools port. Logs go to `$E2E_DIR/NAME/stderr.log` with the sync modules at `--vmodule=...=1`, plus a net log. A restart is a stop and a start with the same name. |
| `cdp.py PORT wait` | Waits until an instance answers. |
| `cdp.py PORT run URL FILE.js [--arg K=V]...` | Opens URL in a background tab, runs the script as an async function body (with the args in `ARGS`), prints its result as JSON, closes the tab. |
| `cdp.py PORT in MATCH FILE.js` | Runs a script in a page already open whose URL contains MATCH, such as the sidebar (`compass-sidebar`). |
| `cdp.py PORT shot URL OUT.png` | Screenshot of a page (for checking the UI by eye). |
| `cdp.py - waitlog FILE REGEX [TIMEOUT]` | Waits until a log line matches. |
| `local_state.py DIR list\|rename\|prefs` | Reads or edits an instance's profiles in Local State while it's stopped (there's no headless way to name a profile), or prints its `compass.sync` prefs with a short hash of the seed. |

On `chrome://compass-sync` (the sync page):

| Script | What it does |
|---|---|
| `start_sync.js --arg server=URL` | "Start sync"; returns the 12 recovery words. |
| `join_recovery.js --arg server=URL --arg words="..."` | Joins a chain with a recovery code. |
| `add_device.js` | "Add a device" on a syncing instance; returns the pairing code. |
| `join_pairing.js --arg server=URL --arg code=N-NNNNNN` | Joins with a pairing code; returns once it waits for the other side. |
| `answer_device.js --arg answer=allow\|deny` | Answers "Add <device>?" on the syncing instance. |
| `cancel_pairing.js` | Cancels the pairing panel. |
| `map_profiles.js [--arg map="NAME=CHAIN NAME;..."]` | Confirms the profile mapping step of a join. |
| `sync_status.js [--arg wait=REGEX]` | The status line, visible sections, pairing panel and invite QR. |
| `page_details.js` | The rest of the page: server field, devices, switches, app menu label. |
| `set_type.js --arg type=NAME --arg on=true\|false` | Flips one "What syncs" switch. |
| `reset_sync.js` | "Reset sync" through its confirm dialog; returns the new words. |

Data:

| Script | Page | What it does |
|---|---|---|
| `add_bookmark.js --arg tag=T` | `chrome://bookmarks` | Adds a test bookmark; returns when (`at`). |
| `find_bookmark.js` | `chrome://bookmarks` | Lists the test bookmarks. |
| `wait_bookmark.js --arg tag=T` | `chrome://bookmarks` | Waits for a test bookmark to arrive; returns when (`at`). |
| `add_password.js --arg tag=T` | `chrome://password-manager` | Saves a test password. |
| `list_passwords.js` | `chrome://password-manager` | Lists saved passwords (no values). |
| `visit.js` | any URL | Opening the URL is the visit; reports what loaded. |
| `query_history.js --arg match=TEXT` | `chrome://history` | Lists matching entries and their visits. |
| `remove_history.js --arg match=TEXT` | `chrome://history` | Removes matching entries (a delete directive each). |
| `other_tabs.js --arg query=TEXT` | `chrome://compass-command-bar` | The command bar's "Tabs on other devices" rows. |
| `sidebar.js --arg op=...` | sidebar (`in compass-sidebar`) | Builds and inspects the sidebar: spaces, folders, entries, moves, closes, archive. The file lists every op. |
| `profiles.js --arg op=dump\|assign` | sidebar | Profiles and their spaces; moves a space to another profile. |
| `navigate.js --arg url=URL` | an open page (`in`) | Navigates that tab as a link would. |
| `trigger_refresh.js` | `chrome://sync-internals` | Asks the engine to fetch now ("Trigger GetUpdates"). |
| `sync_dump.js` | `chrome://sync-internals` | The "Dump status" JSON: types, passphrase state, errors. |

Checking results:

| Helper | What it does |
|---|---|
| `compare_trees.py A.json B.json...` | Compares `sidebar.js op=dump` outputs (what `COMPASS_SPACES` syncs). |
| `compare_profiles.py A.json B.json...` | Compares `profiles.js op=dump` outputs. |
| `netlog_hosts.py NETLOG.json [--urls]` | Hosts an instance contacted, Google ones marked. |
| `swift decode_qr.swift shot.png` | Reads the QR codes in a screenshot (the pairing invite). |

## A typical run

Build Compass first (`tools/compass build`; the helpers default to
`out/Debug/Compass.app`). Then start a local server in another terminal:

```sh
tools/compass sync-server            # go run, http://localhost:8296/v2
tools/compass sync-server --podman   # or the container image
tools/compass sync-server --fresh    # start from an empty database
```

Settings and two shell functions for the run:

```sh
bash
SRC="$(cd tools && python3 -c 'from compass_tool.layout import detect; print(detect().src)')"
export E2E_DIR="$HOME/compass-sync-e2e"          # profiles and logs
export COMPASS_APP="$SRC/out/Debug/Compass.app"  # the default
SERVER="http://localhost:8296/v2"
E2E=tools/sync_e2e
cdp() { python3 -I "$E2E/cdp.py" "$@"; }
sb() { local port="$1"; shift; local args=(); for a in "$@"; do args+=(--arg "$a"); done
       cdp "$port" in compass-sidebar "$E2E/sidebar.js" "${args[@]}"; }
rm -rf "$E2E_DIR"; mkdir -p "$E2E_DIR"
```

A starts a chain. Sync runs in local-sync mode, which Chromium reads when
the profile loads, so starting or joining takes a restart:

```sh
$E2E/instance.sh start A 9301
cdp 9301 wait > /dev/null
cdp 9301 run chrome://compass-sync $E2E/start_sync.js --arg server="$SERVER" | tee "$E2E_DIR/A-start.json"
WORDS="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["words"])' "$E2E_DIR/A-start.json")"
$E2E/instance.sh stop A; $E2E/instance.sh start A 9301; cdp 9301 wait > /dev/null
cdp - waitlog "$E2E_DIR/A/stderr.log" '\[compass-sync\] passphrase in place' 600
```

B joins with the recovery code (or with pairing: `add_device.js` on A,
`join_pairing.js` on B, `answer_device.js --arg answer=allow` on A):

```sh
$E2E/instance.sh start B 9302
cdp 9302 wait > /dev/null
cdp 9302 run chrome://compass-sync $E2E/join_recovery.js --arg server="$SERVER" --arg words="$WORDS"
$E2E/instance.sh stop B; $E2E/instance.sh start B 9302; cdp 9302 wait > /dev/null
cdp - waitlog "$E2E_DIR/B/stderr.log" '\[compass-sync\] SetDecryptionPassphrase -> ' 600   # accepted
cdp - waitlog "$E2E_DIR/B/stderr.log" '\[compass-sync\] spaces: applied .* \(initial merge\)' 600
```

Change something on A and check B:

```sh
cdp 9301 run chrome://bookmarks $E2E/add_bookmark.js --arg tag=from-a
cdp 9302 run chrome://bookmarks $E2E/wait_bookmark.js --arg tag=from-a

sb 9301 op=add-space name=Travel color=blue
sb 9301 op=open space=Travel url=https://example.com/?trip
sb 9302 op=wait space=Travel node=https://example.com/?trip
sb 9301 op=dump > "$E2E_DIR/A-tree.json"
sb 9302 op=dump > "$E2E_DIR/B-tree.json"
python3 -I $E2E/compare_trees.py "$E2E_DIR/A-tree.json" "$E2E_DIR/B-tree.json"   # "same trees"
```

With change notifications working, a change on A reaches B in a second or
two without a refresh. If it doesn't, `trigger_refresh.js` on B separates
"the data didn't sync" from "B wasn't told".

Stop the instances when done:

```sh
$E2E/instance.sh stop A; $E2E/instance.sh stop B
```

## What to look at

- `grep '\[compass-sync\]' "$E2E_DIR/A/stderr.log"`: every POST to the
  server and its result, each state change (passphrase type, whether types
  are held, active types), `spaces:` lines from the sidebar bridge
  ("applied N remote changes", "committing N changed, M deleted"),
  `notifier:` lines (stream connected, changed types, reconnects) and
  `quota:` lines.
- `chrome://sync-internals` (`sync_dump.js`): a custom passphrase
  (`kCustomPassphrase`), no type errors.
- `netlog_hosts.py` on an instance's net log: sync should add no Google
  hosts beyond what a `--disable-sync` instance contacts.
- A third instance (C, D...) for pairing failures (wrong code, deny,
  cancel) and for joining a chain that already has data.

## Tips

- Use bash: zsh doesn't word-split unquoted variables the way the snippets
  expect.
- The auth token is valid within 24 h of the server's clock; check `date -u`
  against the server when using a remote one.
- `--use-mock-keychain` (set by `instance.sh`) gives a fixed os_crypt key,
  so a stored seed decrypts after a restart without a Keychain prompt.
- `--compass-archive-hour-seconds=N` as an extra switch makes an "hour" of
  the archive delay N seconds, to test auto-archive quickly.
- Headless windows never activate, so refresh on focus can't be tested
  this way, and "Show recovery code" (Touch ID) can't be either.
- Pairing joins are rate limited (10 per IP per 10 minutes); restart a
  local server to clear it.
- `chrome://history` groups visits that differ only by query and lists only
  what a search finds, so `query_history.js` searches exact text.
