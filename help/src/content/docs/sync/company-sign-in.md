---
title: Company sign-in
description: Sync through your company's server by signing in with your organization's account.
sidebar:
  order: 6
---

:::note[Coming in the next update]
Company sign-in is new and arrives in the next Compass update. This page
describes how it works once you have that update.
:::

Your company may run its own Compass sync server and allow only its people
to use it. Such a server asks you to **sign in with your organization's
account** (for example, your Microsoft, Google or Okta work account) before
it syncs.

Signing in only decides **who may use the server**. Your data stays
encrypted on your devices exactly as before: neither the server nor your
company's sign-in provider can read it. See
[Encryption and privacy](/sync/encryption/).

## Sign in when you start sync

1. [Turn on sync](/sync/turn-on/) as usual, with your company's server
   address (ask your IT team for it).
2. Compass sees that the server needs sign-in and says **This server
   requires sign-in** with the name of your provider. It opens your
   company's sign-in page in a new tab and shows a short code.
3. Sign in with your work account in that tab.
4. The server's page shows **Allow this device to sync?**, with your
   account, the device's name and a code. **Check that the code is the same
   one Compass shows.** If it is, choose **Allow**. If it isn't, or you
   didn't start this, choose **Cancel**.
5. The page says **You're signed in**. You can close the tab and return to
   Compass, which goes on setting up sync.

If you closed the sign-in tab by mistake, click **Open the sign-in page**.
To stop, click **Cancel**.

:::caution[Always check the code]
Only allow the sign-in if the page shows the same code as Compass. Someone
could send you a sign-in link of their own; the code is how you tell.
:::

## Adding devices doesn't need sign-in

Sign-in belongs to your sync chain, not to each device. Once one device has
signed in, you [add your other devices](/sync/add-a-device/) with a pairing
code as usual, and they sync right away without signing in.

Your other profiles don't need their own sign-in either: they share the
chain's.

## "Sync: sign in needed"

If the app menu shows **Sync: sign in needed**, or the **Sync** page says
**Sign in to** your provider **to sync**, the server wants you to sign in
(again). This can happen when your company's sign-in expires or your access
changes.

1. Open **Settings** and choose **Sync** (or choose **Sync: sign in
   needed** in the app menu).
2. Click **Sign in to** followed by your provider's name (or just **Sign
   in**), and follow the steps above.

Nothing is lost while sync waits: your data stays on your device and syncs
once you've signed in. The **Sync** page then shows **Signed in as** with
your name and email.

## On iPhone, iPad and Android

- **iPhone and iPad:** in **Settings**, the **Sign In** section shows
  **Sign in with** your provider when the server needs it. Compass opens the
  sign-in page and shows the code it must match.
- **Android:** the **Sync** screen shows the sign-in button and, while you
  sign in, the code. **Open the page** opens the sign-in page again.

## Leaving sync doesn't sign out your other devices

**Leave sync** on one device stops syncing that device only. Because
sign-in belongs to the whole chain, your other devices stay signed in and
keep syncing. See [Leave or reset sync](/sync/leave-or-reset/).

**Reset sync** makes a new chain, which has to sign in again.

## For IT teams

To set up company sign-in on your own server, see
[Company sign-in for your server](/self-hosting/company-sign-in/).
