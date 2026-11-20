---
title: Configuration
description: Every environment variable the Compass sync server reads, with its default.
sidebar:
  order: 4
---

The server is configured with environment variables, all optional. With
Docker Compose, put them in `docker-compose.yml` under `sync`'s
`environment` and run `docker compose up -d`; with `docker run`, pass
`-e NAME=value`. Quote numbers in YAML (`"30"`).

A wrong value in most settings stops the server at start with
`Invalid configuration` in its log. The limits marked † are the exception:
a wrong value there is logged and the default is used.

## Basics

| Variable | Default | What it does |
|---|---|---|
| `COMPASS_SYNC_DB` | `/data/compass-sync.db` | The database file |
| `COMPASS_SYNC_ADDR` | `:8295` | The address and port the server listens on |
| `COMPASS_SYNC_PUBLIC_HOST` | the `Host` of each request | The server's public name, e.g. `sync.example.com` (no `https://`, no path). Devices that pair bind it into the pairing, so set it when devices may reach the server by different names (an IP address, a Tailscale name). Company sign-in requires it. |
| `COMPASS_SYNC_TRUSTED_PROXIES` | none | Comma-separated addresses or CIDR ranges of your reverse proxies, whose `X-Forwarded-For` the server believes for the client's address. The bundled `docker-compose.yml` sets Caddy's range. |

## Limits per chain

A chain is one person's set of devices. See
[Operations](/self-hosting/operations/#limits-and-expiry) for what happens
at a limit.

| Variable | Default | What it does |
|---|---|---|
| `COMPASS_SYNC_MAX_ENTITIES` † | `100000` | Items stored per chain (history counts, up to `COMPASS_SYNC_MAX_HISTORY` of it) |
| `COMPASS_SYNC_MAX_BYTES` | `209715200` (200 MiB) | Bytes stored per chain, the shared rooms it owns included; `0` is no limit |
| `COMPASS_SYNC_MAX_HISTORY` † | `30000` | History items (visits, deletions) per chain within the history retention. Past it, new visits stay on the devices. Never more than `COMPASS_SYNC_MAX_ENTITIES`. |
| `COMPASS_SYNC_HISTORY_DAYS` † | `14` | How long history stays on the server after its last change, 1 to 30 days. A device that joins later sees this much of the others' history. |
| `COMPASS_SYNC_TOMBSTONE_DAYS` | `30` | How long deletions are remembered, so devices that were away learn of them; `0` is forever |
| `COMPASS_SYNC_IDLE_CHAIN_DAYS` | `180` | A chain none of whose devices has connected for this many days is deleted; `0` is never |
| `HIGH_DEVICE_LIMIT_CLIENT_IDS` | none | Comma-separated chain IDs allowed 100 devices instead of 50 |

## Access

| Variable | Default | What it does |
|---|---|---|
| `COMPASS_SYNC_ALLOWED_CHAINS` | none (all) | Comma-separated chain IDs to serve; when set, every other chain is refused, except in the shared rooms it was invited to. See [Private server](/self-hosting/private-server/). |
| `BLOCKED_CLIENT_IDS` | none | Comma-separated chain IDs to refuse |

Company sign-in has its own settings, `COMPASS_SYNC_OIDC_*`: see
[Company sign-in](/self-hosting/company-sign-in/#settings).

## Sync timing and change notifications

| Variable | Default | What it does |
|---|---|---|
| `COMPASS_SYNC_POLL_SECONDS` † | `900` | How often devices check for changes, 30 to 86400 seconds. Devices hear of changes at once through change notifications, so this is the fallback. |
| `COMPASS_SYNC_EVENTS_PER_IP` | `50` | Open change-notification streams per client address (each syncing device holds one; there are also at most 20 per chain) |

## Backups

| Variable | Default | What it does |
|---|---|---|
| `COMPASS_SYNC_BACKUP_INTERVAL` | `24h` | How often the server backs up its database, as a duration (`12h`, `90m`; at least a minute). `0` turns backups off. |
| `COMPASS_SYNC_BACKUP_KEEP` | `7` | How many backups are kept; older ones are deleted |
| `COMPASS_SYNC_BACKUP_DIR` | `backups` next to the database (`/data/backups`) | Where backups go |

See [Backups](/self-hosting/operations/#backups).

## Push notifications

For waking phones when a tab is sent to them. They only work with builds of
Compass you sign yourself: see [Push notifications](/self-hosting/push/).

| Variable | Default | What it does |
|---|---|---|
| `COMPASS_SYNC_APNS_KEY` | none | Path to an APNs auth key (`.p8`) |
| `COMPASS_SYNC_APNS_KEY_ID` | none | That key's ID (required with the key) |
| `COMPASS_SYNC_APNS_TEAM_ID` | none | The Apple Developer team ID (required with the key) |
| `COMPASS_SYNC_APNS_TOPICS` | `it.cipi1965.compass` | Comma-separated app IDs (iOS bundle IDs, Android package names) devices may register for |
| `COMPASS_SYNC_FCM_CREDENTIALS` | none | Path to a Firebase service account's JSON key, for Android |

## Operations

| Variable | Default | What it does |
|---|---|---|
| `COMPASS_SYNC_METRICS` | off | `1` serves Prometheus metrics on `/metrics` |
| `COMPASS_SYNC_SHUTDOWN_DELAY` | `0s` | How long to keep serving after SIGTERM before shutting down (for load balancers), as a duration |
| `SENTRY_DSN` | none | Report errors to Sentry |
| `PPROF_ENABLED` | off | `1` serves Go's profiler on `localhost:6061` inside the container |

## Finding a chain ID

Chain IDs are 64 hexadecimal characters (the chain's public key). The
server logs a refused chain's ID (see
[Private server](/self-hosting/private-server/#add-someone)); with company
sign-in, `chains list` shows each signed-in chain and its user.
