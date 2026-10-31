# Compass sync server

The server Compass syncs through: Chromium's sync protocol (a fork of
[brave/go-sync](https://github.com/brave/go-sync), see `NOTICE.md`) with
its data in one SQLite file, plus the relay Compass uses to add a device to
a sync chain. Everything a browser uploads is encrypted on the device with
the chain's key; the server stores blobs it can't read and never sees a
password or an email address. There are no accounts: a chain is an Ed25519
public key, and each request is signed with its private key.

There is no Compass-run instance: you run your own. It needs about 20 MB of
memory and as much disk as your chains store (at most 200 MB each by
default).

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
| `COMPASS_SYNC_MAX_ENTITIES` | `100000` | Live entities per chain (history counts, up to 30000 of it) |
| `COMPASS_SYNC_MAX_BYTES` | `209715200` (200 MiB) | Stored bytes per chain; `0` = no limit |
| `COMPASS_SYNC_TRUSTED_PROXIES` | none | CIDRs whose `X-Forwarded-For` is believed (your reverse proxy) |
| `COMPASS_SYNC_POLL_SECONDS` | `900` | How often devices poll for changes (30-86400); they hear of changes at once through change notifications, so this is the fallback |
| `COMPASS_SYNC_EVENTS_PER_IP` | `50` | Open change-notification streams per client address |
| `COMPASS_SYNC_SHUTDOWN_DELAY` | `0s` | Wait before closing on SIGTERM (for load balancers) |
| `COMPASS_SYNC_METRICS` | off | `1` serves Prometheus metrics on `/metrics` |
| `COMPASS_SYNC_PUBLIC_HOST` | the request's `Host` | The server's name both devices bind when pairing (e.g. `sync.example.com`); set it when devices reach the server by different names (IP, Tailscale name) |
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

## Backups

The database is the file `compass-sync.db` in the `/data` volume. Don't copy
it with `cp` while the server runs (SQLite also has `-wal` and `-shm` files
next to it, and a copy taken mid-write can be torn). Instead let the server
write a consistent copy, which is safe at any time:

```sh
docker compose exec -T sync /compass-sync-server backup - > compass-sync-$(date +%F).db
```

(`-T` matters: without it the terminal mangles the bytes.) The copy is
compacted, and a cron job running that line is a complete backup plan.
With `sqlite3` installed on the host, `sqlite3 <volume dir>/compass-sync.db
".backup ./compass-sync-$(date +%F).db"` does the same
(`docker volume inspect compass-sync_sync-data` shows the directory).

To restore, stop the server, put the backup in the volume as
`compass-sync.db` (removing any `compass-sync.db-wal` and `-shm`), and
start it again.

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
