---
title: Leave or reset sync
description: Stop syncing a device, start over with a new chain, or move to another sync server.
sidebar:
  order: 8
---

Both options are at the bottom of the **Sync** page, under **Leave or
reset**.

## Leave sync

Leaving stops syncing **this device**. Its data stays on it, and your other
devices go on syncing without it.

1. Open **Settings** and choose **Sync**.
2. Click **Leave sync**, then **Leave sync** again to confirm.
3. Compass restarts to finish.

Leaving applies to every profile on this device. To sync again later, join
your chain again with a pairing code or your recovery code (see
[Add a device](/sync/add-a-device/)).

- **iPhone and iPad:** in **Settings**, tap **Leave Sync**. Your spaces stay
  on the device.
- **Android:** on the **Sync** screen, tap **Leave the chain**.

## Reset sync

Resetting makes a **new chain** with a new recovery code. Use it when you
no longer trust a device in your chain (a lost or stolen computer, for
example): a device you leave out of the new chain can't read anything you
sync from then on.

1. Open **Settings** and choose **Sync**.
2. Click **Reset sync**, then **Reset sync** again to confirm.
3. Save your **new** recovery code, then click **I saved it**.
4. Click **Restart Compass**. This device's data goes up to the new chain.
5. [Add your other devices](/sync/add-a-device/) to the new chain. Until
   you do, they stay on the old chain and don't see this device's changes.

:::caution
After a reset, your old recovery code no longer works on this device. Use
the new one.
:::

The old chain's data stays on the server until no device has used it for a
while, then the server deletes it.

On iPhone, iPad and Android you can't reset sync. Do it on a computer, then
add your phone or tablet again.

:::note[In the next update]
An iPhone or iPad can start a new chain of its own: tap **Leave Sync**,
then **Start Sync on This iPhone** (or **iPad**). It's a new chain, with a
new recovery code, that your other devices must join.
:::

## Use another sync server

You can't change the server of a chain in place. To move to another server:

1. On every device, [leave sync](#leave-sync).
2. On one computer, [start sync](/sync/turn-on/) with the new server's
   address. This makes a new chain, with a new recovery code.
3. [Add your other devices](/sync/add-a-device/) with the new server's
   address.

Your data comes along, because each device keeps its own copy and uploads
it to the new chain.

## Company sign-in and leaving

If your sync server uses [company sign-in](/sync/company-sign-in/),
leaving sync on one device doesn't sign out your other devices: sign-in
belongs to the whole chain, and they keep syncing. A reset makes a new
chain, which signs in again.
