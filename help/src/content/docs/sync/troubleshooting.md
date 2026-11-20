---
title: Troubleshooting sync
description: What Compass's sync messages mean and what to do about them.
sidebar:
  order: 7
---

On a computer, the sync status shows in two places: a row in the app menu
(**⋯**), above **Settings**, and at the top of the **Sync** page in Settings,
with a colored dot: green when all is well, yellow when Compass is waiting
for something, red for an error.

## App menu

| The menu says | What it means |
|---|---|
| **Synced** | All is well. |
| **Syncing…** | Sync is starting or catching up. |
| **Sync is off** | This device isn't in a sync chain. See [Turn on sync](/sync/turn-on/). |
| **Sync: restart to finish** | Restart Compass to finish starting, joining, leaving or resetting sync. |
| **Sync error** | Something stopped sync. Open the **Sync** page to see what. |
| **Sync: sign in needed** | Your company's server wants you to sign in. See [Company sign-in](/sync/company-sign-in/). |
| **Sync: server full** | The server's storage limit for your chain is reached. See below. |
| **Sync: paused by server** | The server is holding this device back for a while. See below. |
| **Synced (history full)** | Everything syncs except your newest history. See below. |

Choose the row to open the **Sync** page.

## Messages on the Sync page

### "Restart Compass to start syncing."

Starting or joining sync takes a restart. Click **Restart Compass**.
Likewise for **Restart Compass to finish leaving sync.** and **Restart
Compass to start the new sync chain.**

### "Can't reach the server"

Compass can't connect to the sync server. Check your internet connection.
If you run your own server, check that it's running and that the address
is right. Compass keeps trying by itself, and your changes stay on this
device until it can sync them.

### "The server refused this device's key"

The server doesn't accept this device's chain, for example because the
server's owner removed it or the server was replaced. Leave sync, then join
again with a pairing code from a device that still syncs, or start a new
chain. See [Leave or reset sync](/sync/leave-or-reset/).

### "Sync stopped: …"

Sync stopped for the reason that follows. A common one: no device used the
chain for a long time, and the server removed it (by default, servers do
this after about six months). Your data on this device stays. Leave sync,
then start a new chain or join one again.

### "The data on the server is encrypted for another chain."

This device's key doesn't match the data on the server, usually because
the chain was reset on another device. Leave sync, then join again with a
new code from a device that syncs.

### "Couldn't read this device's sync key."

Compass couldn't open the secret it stores for sync on this device. Leave
sync and join again.

### "The sync server is full for this chain"

Your chain reached the storage limit of its server. New changes stay on
this device. Remove data you don't need (for example, old history), or ask
the server's owner to raise its limits.

### "The sync server is holding this device back"

The server is limiting this device, because the chain has too many devices
or sent too many requests. Compass tries again by itself; you don't need to
do anything.

### "History is full on the server"

Your other data syncs, but new history visits stay on this device until
older ones expire on the server.

### "Choose how this computer's other profiles sync."

This computer has several profiles. Choose which of your chain's profiles
each one syncs with, then click **Continue**. See
[Profiles and sync](/sync/profiles/).

### "Sync is turned off for this profile."

Sync isn't available in this profile.

## Pairing messages

On the device you're adding:

| Message | What to do |
|---|---|
| **Enter the code as shown, like 7-482913.** | Type the whole code, with the number before the dash. |
| **No such code on this server, or it expired.** | Check that both devices use the same server address, then ask for a new code. |
| **Wrong code. Ask the other device for a new one.** | A code allows a single try. Click **Add a device** again on the other device. |
| **Code expired. Ask for a new one.** | Codes last 5 minutes. |
| **That code was already used. Ask for a new one.** | Each code adds one device. |
| **Not added: the other device said no.** | The other device chose **Deny**. |
| **The other device canceled. Ask for a new code.** | Pairing was canceled on the other device. |
| **Too many attempts. Try again in a few minutes.** | Wait a few minutes, then try again. |
| **Couldn't reach the server.** | Check your connection and the server address. |

On the device showing the code, **Device added. It starts syncing after a
restart.** means it worked: restart Compass on the new device.

## On iPhone and iPad

**Settings** shows the sync **Status** (**On**, **Syncing…**, **Off** or
**Sign-in needed**) and when it **Last synced**. Tap **Sync Now** to sync
right away.

| Message | What to do |
|---|---|
| **Can't reach the sync server** | Check your connection. Compass tries again later. |
| **The sync server refused this device's key.** | Leave sync and join again with a new code. |
| **This chain was reset on another device. Join it again.** | Leave sync, then join with a new code from a device that syncs. |
| **No device has started sync on this chain yet.** | Make sure sync finished starting on your computer: restart Compass there if it asks. |
| **That code isn't open any more. Get a new one on your Mac.** | Click **Add a device** again on your computer. |
| **Wrong code. Your Mac shows a new one.** | Type the new code. |
| **That code was already used.** / **The code expired.** | Get a new code on your computer. |
| **Too many tries. Wait a few minutes.** | Wait, then try again. |
| **Check the 12 words.** | A word of your recovery code is wrong or missing. |

:::note[In the next update]
Compass for iPhone and iPad uses the computer's words when joining (for
example **Wrong code. Ask the other device for a new one.** or **Code
expired. Ask for a new one.**) and, when it adds a device itself, the same
messages as the device showing the code above. A chain with no started
device no longer stops it: the first device that syncs starts it.
:::

## Still stuck?

- Check that every device uses exactly the same server address, ending in
  `/v2`.
- If you run your own server, see [Run your own sync server](/self-hosting/).
- Report a problem on
  [GitHub](https://github.com/Compass-Browser/Compass/issues).
