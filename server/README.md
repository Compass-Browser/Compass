# Compass sync server

The server Compass syncs through: Chromium's sync protocol (a fork of
[brave/go-sync](https://github.com/brave/go-sync), see `NOTICE.md`) with
its data in one SQLite file, plus the relay Compass uses to add a device to
a sync chain. Everything a browser uploads is encrypted on the device with
the chain's key; the server stores blobs it can't read and never sees a
password or an email address. There are no accounts: a chain is an Ed25519
public key, and each request is signed with its private key.

A public instance runs at https://sync.example.org, or you can run
your own. It needs about 20 MB of memory and as much disk as your chains
store (at most 200 MB each by default).

In Compass, the server URL is the server's address followed by `/v2`, e.g.
`https://sync.example.com/v2`.

## On a VPS, with HTTPS

You need a machine with Docker (or Podman with `podman compose`), a domain
name pointing at it, and ports 80 and 443 open.

```sh
git clone https://github.com/<you>/compass.git
cd compass/server
# Put your domain in place of sync.example.com:
sed -i 's/sync.example.com/sync.your-domain.org/' Caddyfile
docker compose up -d
docker compose logs -f
```

Caddy fetches a certificate on the first request. Check it with
`curl https://sync.your-domain.org/healthz` (answers `ok`).

## On a home server, behind Tailscale

If your devices are all on your tailnet, the server doesn't need to be on
the internet. Run the container alone and publish its port on the
machine's Tailscale address only:

```sh
docker build -t compass-sync-server .
docker run -d --name compass-sync --restart unless-stopped \
  -p 100.x.y.z:8295:8295 -v compass-sync-data:/data compass-sync-server
```

(`100.x.y.z` is the machine's address from `tailscale ip -4`.) The server
URL is then `http://100.x.y.z:8295/v2`, or with MagicDNS
`http://<machine-name>:8295/v2`. Plain HTTP is fine here: Tailscale encrypts
the traffic, and the data is encrypted by Compass anyway.

For HTTPS inside the tailnet, `tailscale serve --bg --https=443
http://127.0.0.1:8295` (with `-p 127.0.0.1:8295:8295` above) gives
`https://<machine-name>.<tailnet>.ts.net/v2`. Then also set
`COMPASS_SYNC_TRUSTED_PROXIES=127.0.0.1` so pairing rate limits see the
real device addresses.

## Configuration

Environment variables (all optional):

| Variable | Default | |
|---|---|---|
| `COMPASS_SYNC_DB` | `/data/compass-sync.db` | Database file |
| `COMPASS_SYNC_ADDR` | `:8295` | Listen address |
| `COMPASS_SYNC_MAX_ENTITIES` | `100000` | Live entities per chain (history counts, up to `COMPASS_SYNC_MAX_HISTORY` of it) |
| `COMPASS_SYNC_MAX_BYTES` | `209715200` (200 MiB) | Stored bytes per chain; `0` = no limit |
| `COMPASS_SYNC_MAX_HISTORY` | `30000` | History items (visits, deletions) per chain within the retention; past it new visits stay on the devices (never more than `COMPASS_SYNC_MAX_ENTITIES`) |
| `COMPASS_SYNC_HISTORY_DAYS` | `14` | How long history stays on the server after its last change (1-30; a device joining later sees this much of the others' history) |
| `COMPASS_SYNC_TOMBSTONE_DAYS` | `30` | Deletions are kept this long so devices that were away learn of them; `0` = forever |
| `COMPASS_SYNC_IDLE_CHAIN_DAYS` | `180` | A chain none of whose devices has talked to the server for this long is deleted (a device coming back is told to start again); `0` = never |
| `COMPASS_SYNC_BACKUP_INTERVAL` | `24h` | How often the server backs up its database (Go duration; `0` = no backups) |
| `COMPASS_SYNC_BACKUP_KEEP` | `7` | Backups kept; older ones are deleted |
| `COMPASS_SYNC_BACKUP_DIR` | `backups` next to the database | Where backups go |
| `COMPASS_SYNC_TRUSTED_PROXIES` | none | CIDRs whose `X-Forwarded-For` is believed (your reverse proxy) |
| `COMPASS_SYNC_POLL_SECONDS` | `900` | How often devices poll for changes (30-86400); they hear of changes at once through change notifications, so this is the fallback |
| `COMPASS_SYNC_EVENTS_PER_IP` | `50` | Open change-notification streams per client address |
| `COMPASS_SYNC_SHUTDOWN_DELAY` | `0s` | Wait before closing on SIGTERM (for load balancers) |
| `COMPASS_SYNC_METRICS` | off | `1` serves Prometheus metrics on `/metrics` |
| `COMPASS_SYNC_PUBLIC_HOST` | the request's `Host` | The server's name both devices bind when pairing (e.g. `sync.example.com`); set it when devices reach the server by different names (IP, Tailscale name) |
| `COMPASS_SYNC_APNS_KEY` | none | An APNs auth key (`.p8`), to wake iPhones when a tab is sent to them (see "Push to iPhones"); without it, sent tabs arrive at the next sync |
| `COMPASS_SYNC_APNS_KEY_ID` | none | That key's ID (required with the key) |
| `COMPASS_SYNC_APNS_TEAM_ID` | none | The Apple Developer team ID (required with the key) |
| `COMPASS_SYNC_APNS_TOPICS` | `it.cipi1965.compass` | Comma-separated bundle IDs devices may register for |
| `BLOCKED_CLIENT_IDS` | none | Comma-separated chain IDs (hex public keys) to refuse |

Never list a proxy as trusted unless every request reaches the server
through it: anyone who can connect directly could then choose the address
the pairing rate limits see.

## Change notifications and reverse proxies

Each syncing device keeps one request open to `/v2/compass/events`
(Server-Sent Events) and asks for changes as soon as another device of its
chain commits; the server sends only which data types changed. A comment
line every 25 s keeps the stream alive. A proxy in front must pass the
response through unbuffered and not time it out:

- **Caddy**: works as is (`reverse_proxy` flushes `text/event-stream` at
  once). The bundled `Caddyfile` also sets `flush_interval -1`, which you can
  copy into your own site block. Reloading Caddy closes open streams; the
  devices reconnect within seconds.
- **nginx**: `proxy_buffering off;` (the server also sends
  `X-Accel-Buffering: no`) and `proxy_read_timeout` above 25 s.
- **`tailscale serve`**: works as is.

Limits: 20 open streams per chain and 50 per client address
(`COMPASS_SYNC_EVENTS_PER_IP`). Behind a proxy, set
`COMPASS_SYNC_TRUSTED_PROXIES`, or every device has the proxy's address and
the 50 are for all devices together. If the stream can't be held open, devices still sync on their
poll (`COMPASS_SYNC_POLL_SECONDS`), on window focus and on wake.

## Push to iPhones

When a tab is sent to an iPhone ("Send to Your Devices" on a Mac), the
server can wake it through Apple's push service so the notification shows
at once, not at the phone's next sync. The phone registers its push token
per chain (`POST /v2/compass/push?client_id=<cache GUID>` with
`{"token","environment","topic"}`, `DELETE` to stop); a commit that adds a
SEND_TAB_TO_SELF entry sends every other registered device of the chain an
alert (the entry is encrypted, so the server can't tell which phone it is
for; the phone's notification extension decrypts it). A token Apple reports
as gone is dropped.

To turn it on, make a key in the Apple Developer portal (Certificates,
Identifiers & Profiles → Keys → Apple Push Notifications service), put the
`.p8` file where the container can read it (e.g. `./data/apns.p8`, mounted
at `/data/apns.p8`) and set `COMPASS_SYNC_APNS_KEY`, `COMPASS_SYNC_APNS_KEY_ID`
and `COMPASS_SYNC_APNS_TEAM_ID`. Without a key, registrations are kept and
nothing is pushed.

## Limits

Each chain may hold `COMPASS_SYNC_MAX_ENTITIES` items and
`COMPASS_SYNC_MAX_BYTES` bytes, and `COMPASS_SYNC_MAX_HISTORY` history items
within the last `COMPASS_SYNC_HISTORY_DAYS`. A device that hits one keeps
its data; Compass shows "Sync: server full" (or "History full") in its menu
and on `chrome://compass-sync`, which reads `GET /v2/compass/quota`. Deleted
items are forgotten after `COMPASS_SYNC_TOMBSTONE_DAYS`, and chains nobody
uses after `COMPASS_SYNC_IDLE_CHAIN_DAYS`.

## Backups

The server backs itself up: once a day (`COMPASS_SYNC_BACKUP_INTERVAL`) it
writes a consistent, compacted copy of its database to `/data/backups`
(`compass-sync-<UTC time>.db`) and keeps the newest 7
(`COMPASS_SYNC_BACKUP_KEEP`). The first one is made at start if the newest on
disk is older than the interval. They sit on the same disk as the database,
so copy `/data/backups` elsewhere now and then (rsync, restic, the VPS's
snapshots).

The database is the file `compass-sync.db` in the `/data` volume. Don't copy
it with `cp` while the server runs (SQLite also has `-wal` and `-shm` files
next to it, and a copy taken mid-write can be torn). To take a copy by hand,
let the server write one, which is safe at any time:

```sh
docker compose exec -T sync /compass-sync-server backup - > compass-sync-$(date +%F).db
```

(`-T` matters: without it the terminal mangles the bytes.) The copy is
compacted, and a cron job running that line is a complete backup plan.
With `sqlite3` installed on the host, `sqlite3 <volume dir>/compass-sync.db
".backup ./compass-sync-$(date +%F).db"` does the same
(`docker volume inspect compass-sync_sync-data` shows the directory).

To restore:

```sh
docker compose stop sync
cd <volume dir>                     # holds compass-sync.db and backups/
mkdir -p replaced && mv compass-sync.db* replaced/   # the db, -wal and -shm
cp backups/compass-sync-20261031T220000Z.db compass-sync.db
chown 65532:65532 compass-sync.db   # the image's user
docker compose start sync
curl -fsS http://127.0.0.1:8295/healthz   # or your public URL
```

Devices carry on from where the backup was: changes made after it come
back as they commit again, and devices that synced later may show those
changes as conflicts they resolve themselves. Delete `replaced/` once all
is well.

Losing the server isn't losing your data: every device keeps its own copy,
and a new, empty server gets everything again from the devices
(Compass: "Reset sync"). Backups save the devices that upload, and keep
history that no device has any more.

## Upgrades

```sh
git pull
docker compose build sync
docker compose up -d
```

For a server on another machine that builds from a copy of `server/` (as
the public instance does), `deploy/update-remote.sh` does all of it from
your checkout: backs up the database, rsyncs `server/` to `<dir>/src`,
builds with `COMMIT` set to the checkout's short SHA, restarts, and checks
`/health-check` names that commit and `/healthz` answers:

```sh
server/deploy/update-remote.sh --host <your server> --port <ssh port> \
  --public-url <url>
```

`--user`, `--dir` and `--service` default to `root`, `/opt/compass-sync`
and `sync`. `--dry-run` prints the commands without running them. It uses your own ssh
keys and config; nothing secret is in the script.

The server migrates the database on start. A server never opens a database
written by a newer version (it refuses to start), so take a backup before
upgrading if you may want to go back.

## Development

From the repository root, `tools/compass sync-server` runs it locally on
port 8296 (`--podman` to run the image instead of `go run`). Tests:

```sh
cd server && go test ./...
```

`docs/design/sync.md` in the repository describes the protocol, the token
format and the pairing API.
