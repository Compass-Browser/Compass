---
title: Run your own sync server
description: What the Compass sync server does, what it can and can't see, what it needs, and how Compass connects to it.
sidebar:
  order: 1
  label: Overview
---

Compass syncs your spaces, tabs, bookmarks, history, passwords and settings
between your devices through a sync server. The project runs a public one,
and you can run your own: it's one small program in a Docker container,
with all its data in a single SQLite file.

## What the server sees

Everything Compass uploads is encrypted on your device with your sync
chain's key before it leaves. The server stores blobs it can't read. It
never sees a password, a page you visited or an email address, and there
are no accounts: a sync chain (one person's set of devices) is identified
by a public key, and each request is signed with the matching private key,
which never leaves your devices.

What the server does know: that a chain exists, how many items and bytes it
stores, which data types changed and when, and the IP addresses devices
connect from.

Losing the server doesn't lose your data either. Every device keeps its own
copy, and a new, empty server gets everything again from the devices.

## Why run your own

The Compass project's server (`sync.compassbrowser.org`) is invite-only for
now, so most people syncing need a server of their own, or one run by
someone they trust. Run your own if you want:

- the data on a machine you control, or never on the internet at all (a
  home server reached over [Tailscale](/self-hosting/other-setups/#home-server-with-tailscale));
- a server only your family or friends can use
  ([private server](/self-hosting/private-server/));
- a server for your company, where people sign in with the company's
  identity provider ([company sign-in](/self-hosting/company-sign-in/));
- your own limits on how much each chain may store.

## What you need

- A Linux machine with Docker (or Podman). A small VPS is plenty: the
  server needs about 20 MB of memory.
- Disk for what your chains store: at most 200 MB per chain by default,
  usually far less, plus the daily backups the server keeps of itself.
- For a server on the internet: a domain name and ports 80 and 443 open,
  so the bundled Caddy can get an HTTPS certificate.

The server image is published for `linux/amd64` and `linux/arm64`
(a Raspberry Pi 4 or 5 with a 64-bit OS works).

## The server URL

In Compass, the server URL is the server's address followed by `/v2`:

```
https://sync.example.com/v2
```

Enter it in the **Sync server** field when you turn on sync (in Compass's
settings, under Sync). Every device of a chain uses the same server. To move
an existing chain to another server, leave sync on each device and join
again with the new URL; the devices upload their data to it.

## Next steps

1. [Install with Docker](/self-hosting/docker/): the server and Caddy for
   HTTPS, in a few commands.
2. Or use [another setup](/self-hosting/other-setups/): your own reverse
   proxy, Tailscale, Podman, or building from source.
3. Look through the [configuration](/self-hosting/configuration/) and
   [operations](/self-hosting/operations/) (backups, upgrades, monitoring).
