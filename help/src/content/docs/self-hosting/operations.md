---
title: Operations
description: Backups and restores, upgrades, monitoring, limits, logs and troubleshooting for a Compass sync server.
sidebar:
  order: 8
---

The commands here assume the bundled `docker-compose.yml`, run from its
directory. With `docker run`, use `docker exec compass-sync ...` and
`docker logs compass-sync` instead.

## Backups

**The server backs itself up.** Once a day it writes a consistent,
compacted copy of its database to `/data/backups` in its volume, named
`compass-sync-<UTC time>.db`, and keeps the newest 7. The first one is made
at start if the newest on disk is older than a day. Change it with
`COMPASS_SYNC_BACKUP_INTERVAL`, `COMPASS_SYNC_BACKUP_KEEP` and
`COMPASS_SYNC_BACKUP_DIR` ([Configuration](/self-hosting/configuration/#backups)).

These sit on the same disk as the database, so copy them elsewhere now and
then (rsync, restic, your provider's snapshots). To find them on the host:

```sh
docker volume inspect -f '{{ .Mountpoint }}' compass-sync_sync-data
```

**A copy on demand.** Ask the running server for one, which is safe at any
time:

```sh
docker compose exec -T sync /compass-sync-server backup - > compass-sync-$(date +%F).db
```

`-T` matters: without it the terminal mangles the bytes. A cron job running
that line, plus copying the file off the machine, is a complete backup
plan.

:::caution
Don't copy `compass-sync.db` with `cp` while the server runs: SQLite keeps
recent changes in `-wal` and `-shm` files next to it, and a copy taken
mid-write can be torn. Use the backups or the command above.
:::

### Restore

As root (the volume's directory belongs to root), in the directory with
`docker-compose.yml`:

```sh
docker compose stop sync
cd "$(docker volume inspect -f '{{ .Mountpoint }}' compass-sync_sync-data)"
mkdir -p replaced && mv compass-sync.db* replaced/      # the db, -wal and -shm
cp backups/compass-sync-20261031T220000Z.db compass-sync.db
chown 65532:65532 compass-sync.db                       # the image's user
cd - && docker compose start sync
curl -fsS https://sync.example.com/healthz
```

Devices carry on from where the backup was: changes made after it come
back as the devices upload them again, and devices that synced later may
show those changes as conflicts they resolve themselves. Delete `replaced/`
once all is well.

:::note
Losing the server isn't losing your data. Every device keeps its own copy,
and a new, empty server gets everything again from the devices (in Compass,
"Reset sync"). Backups spare the devices the upload, and keep history that
no device has any more.
:::

## Upgrading

```sh
docker compose pull
docker compose up -d
```

The server updates its database on start; there's nothing to migrate by
hand. To stay on a known build, use a `sha-<short commit>` tag of the image
instead of `latest`.

A server never opens a database written by a newer version: it refuses to
start (`database schema N is newer than this server`). So you can't go back
to an older image after an upgrade changed the database, unless you restore
a backup made before it. Take one before upgrading
(`backup -`, above) if you may want to go back.

Restarting closes the devices' open connections; they reconnect within
seconds.

## Monitoring

- **`/healthz`** answers `ok` when the server runs and its database
  answers, `503` otherwise. The container's health check uses it
  (`docker compose ps` shows `healthy`). Point an uptime checker at
  `https://sync.example.com/healthz`.
- **`/health-check`** answers JSON with the server's version, commit and
  build time.
- **`/metrics`**: with `COMPASS_SYNC_METRICS: "1"`, Prometheus metrics
  (requests, database calls, Go runtime).

:::caution
The bundled `Caddyfile` sends every path to the server, so `/metrics`
would be public. Scrape it on the internal network, or answer it in Caddy
before the proxy:

```txt
sync.example.com {
	respond /metrics 404
	reverse_proxy sync:8295 {
		flush_interval -1
	}
}
```
:::

## Logs

```sh
docker compose logs -f sync
docker compose logs --since 1h sync
```

The server logs JSON lines; refused requests show as
`Authorization failed` with the reason.

## Limits and expiry

Each chain may store `COMPASS_SYNC_MAX_ENTITIES` items (100,000) and
`COMPASS_SYNC_MAX_BYTES` bytes (200 MiB), and at most
`COMPASS_SYNC_MAX_HISTORY` history items (30,000) from the last
`COMPASS_SYNC_HISTORY_DAYS` (14). A chain may have up to 50 devices.

- A device that hits the item or byte limit keeps its data. Compass shows
  "Sync: server full" in its menu and sync settings.
- At the history limit, other data keeps syncing and new visits stay on the
  device they were made on; Compass shows "History full".

Expiry runs every hour:

- History older than `COMPASS_SYNC_HISTORY_DAYS` is deleted from the server
  (the devices keep their own).
- Deletions are remembered for `COMPASS_SYNC_TOMBSTONE_DAYS` (30) so devices
  that were away learn of them.
- A chain none of whose devices has connected for
  `COMPASS_SYNC_IDLE_CHAIN_DAYS` (180) is deleted. A device that comes back
  later is told to stop syncing; its own data stays, and it can start sync
  again.

## Troubleshooting

**`curl https://sync.example.com/healthz` fails.** Check `docker compose ps`
and `docker compose logs caddy`: most often the DNS record doesn't point at
the machine yet, or port 80 or 443 is closed, so Caddy can't get a
certificate. `Invalid configuration` in the sync log names the setting
that's wrong.

**A device gets `401` / "the server refused this device's key".** Look for
`Authorization failed` in the log. `chain not allowed: <id>` means the
server is [private](/self-hosting/private-server/) and the chain isn't on
the list; `this client ID is blocked` means it's in `BLOCKED_CLIENT_IDS`.
A token that's out of date can also mean the device's clock is off by more
than a day.

**`401 {"error":"login_required"}`.** [Company sign-in](/self-hosting/company-sign-in/)
is on and the chain isn't signed in (or its session ended, or the user was
revoked). Compass asks to sign in. If the sign-in page never opens, check
the log for `Sign-in: identity provider still unreachable`.

**Pairing says the code is wrong every time.** The two devices reach the
server by different names (an IP address and a host name, or a Tailscale
name). Use the same server URL on both, or set `COMPASS_SYNC_PUBLIC_HOST`.
Behind your own proxy, make sure it passes the `Host` header through.

**Pairing hits "too many attempts" (`429`).** Behind a reverse proxy
without `COMPASS_SYNC_TRUSTED_PROXIES`, every device has the proxy's
address and shares one limit (10 joins per 10 minutes). Set it to the
proxy's address.

**Changes take minutes to show up on other devices.** The change
notification stream (`/v2/compass/events`) isn't getting through: a proxy
is buffering it or closing it. Turn buffering off and keep idle timeouts
above 30 seconds (see [your own reverse proxy](/self-hosting/other-setups/#behind-your-own-reverse-proxy)).
`curl -i https://sync.example.com/v2/compass/events` should get the
server's own `401` (`invalid sync token`) at once, which shows the path
reaches it. Devices also fall back to syncing every 15 minutes, on focus and on wake.
