---
title: Private server (friends only)
description: Serve only the sync chains you choose, and add or remove people.
sidebar:
  order: 5
---

The server URL isn't a secret: anyone who knows it can start a sync chain on
your server. To serve only the people you choose, list their chains in
`COMPASS_SYNC_ALLOWED_CHAINS`. A chain is one person's set of devices, so
one entry covers every device they pair. A Compass profile that syncs as
its own account is a chain of its own, with its own ID.
Every other chain is refused with `401`, for syncing, pairing, push, quota
and change notifications alike.

For a company with an identity provider, [company sign-in](/self-hosting/company-sign-in/)
does this without collecting chain IDs.

## Add someone

1. They turn on sync in Compass with your server URL. The server refuses
   them and logs their chain ID:

   ```sh
   docker compose logs sync | grep "chain not allowed"
   ```

   The line ends with `chain not allowed: ` and the ID, 64 hexadecimal
   characters.

2. Add the ID to `docker-compose.yml`, under `sync`'s `environment`
   (comma-separated for several people):

   ```yaml
         COMPASS_SYNC_ALLOWED_CHAINS: "3f9a…e1,7c02…4b"
   ```

   and apply it:

   ```sh
   docker compose up -d
   ```

   The restart takes seconds, and devices reconnect by themselves.

3. They sync again, or wait for Compass's next retry.

Your own chain gets in the same way.

## Make a running server private

To turn an open server into a private one without cutting off the people
already using it, start the list with the chains it already holds. With
`sqlite3` installed on the host:

```sh
sudo sqlite3 "$(docker volume inspect -f '{{ .Mountpoint }}' compass-sync_sync-data)/compass-sync.db" \
  "SELECT DISTINCT lower(client_id) FROM item_counts"
```

`compass-sync_sync-data` is the volume the bundled `docker-compose.yml`
makes; `docker volume ls` lists yours.

## Remove someone

Take their ID off the list (or add it to `BLOCKED_CLIENT_IDS`) and run
`docker compose up -d`. Their devices stop syncing and keep their own data.
What they stored on the server stays until it expires as an idle chain
(`COMPASS_SYNC_IDLE_CHAIN_DAYS`, 180 days by default).
