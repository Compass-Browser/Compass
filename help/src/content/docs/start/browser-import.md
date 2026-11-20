---
title: Import from another browser
description: Bring your bookmarks, open tabs, history and passwords from Chrome, Safari, Firefox, Edge, Brave and other browsers into Compass spaces.
sidebar:
  order: 4
---

Compass can bring over what you have in other browsers on the same
computer: bookmarks, the tabs you have open, your history and saved
passwords. Each browser profile becomes a new [space](/guide/spaces/), so
nothing gets mixed into your existing ones.

Coming from Arc? It has its own import, which also keeps you signed in to
your sites: see [Coming from Arc](/start/coming-from-arc/).

## Which browsers

| Browser | Mac | Windows | Linux |
|---|---|---|---|
| Chrome | Yes | Yes | Yes |
| Microsoft Edge | Yes | Yes | Yes |
| Brave | Yes | Yes | Yes |
| Vivaldi | Yes | Yes | Yes |
| Dia | Yes | Yes | |
| Firefox | Yes | Yes | Yes |
| Zen | Yes | Yes | Yes |
| Safari | Yes | | |

Importing is available in Compass for Mac, Windows and Linux. To get your
imported spaces on iPhone, iPad or Android, [turn on sync](/sync/turn-on/).

## Import

The first time you open Compass, it offers to import if it finds another
browser on your computer. To import later:

1. Quit the browser you're importing from. Compass can't read its files
   while it's running.
2. In Compass, open the command bar (<kbd>⌘</kbd> <kbd>T</kbd> /
   <kbd>Ctrl</kbd>+<kbd>T</kbd>) and choose **Import from Another
   Browser**, or open **Settings** and go to **Import from Another
   Browser**.
3. Under **Browsers on this computer**, check the profiles you want. For
   each one, choose what to bring: **Bookmarks to Pinned**, **Open tabs to
   Today**, **History** and **Passwords** (History and Passwords appear
   only where Compass can import them).
4. Click **Import**.
5. When it's done, Compass shows a summary for each profile. Click **Open
   my spaces** to see them, or **Import more** to import another one.

If a browser is still open, its profile is greyed out with "Quit it to
import from it". Quit it, then click **Check again**.

:::note[On a Mac]
- To import passwords from a Chromium-based browser (Chrome, Edge, Brave,
  Vivaldi, Dia), macOS asks whether Compass may use that browser's "Safe
  Storage" key. Choose **Allow** so Compass can read them.
- To import from Safari, Compass needs Full Disk Access: in System
  Settings, go to **Privacy & Security**, **Full Disk Access**, and turn on
  Compass. Then import again.
:::

## Where things go

- **A new space for each browser profile**, named after the browser, such
  as "Chrome", or "Chrome – Work" when the browser has several profiles.
  It gets a color none of your spaces has yet.
- **Bookmarks go to Pinned.** The bookmarks bar (Safari's Favorites, or
  Firefox's toolbar) comes first, at the top level; the browser's other
  bookmark folders, such as "Other bookmarks" or "Bookmarks Menu", follow
  as Pinned folders.
- **Open tabs go to Today**, in their order. Named tab groups become Today
  folders, and pinned tabs come first. They won't be swept to the archive
  right away.
- **History** joins your Compass history, and **passwords** your Compass
  passwords.

Some browsers have spaces of their own, and those become Compass spaces too:

- **Safari's tab groups**: one space each, with the group's tabs in Today
  and its start page's favorites in Pinned.
- **Vivaldi's workspaces**: one space each, with their name and emoji.
- **Zen's spaces**: one space each, with their name, emoji and colors.
  Zen's pinned tabs and folders go to Pinned, its essentials to your
  [Favorites](/guide/sidebar/), and its bookmarks to an extra "Zen" space.
- **Dia's pinned tabs** go to Pinned.

## What doesn't come over

- **Logins (cookies):** you'll need to sign in to your sites again in
  Compass. (Arc is the exception; see [Coming from Arc](/start/coming-from-arc/).)
- **Extensions:** install them again from the Chrome Web Store; see
  [Extensions](/guide/extensions/).
- Settings, autofill data, Firefox containers, and Zen's Boosts and Mods.
- **Safari's passwords**, which live in your Mac's Keychain, and
  **Firefox's and Zen's passwords on a Mac**.
- Safari profiles other than the default one, except their tab groups.
- On Windows, some newer Chrome passwords can only be opened by Chrome
  itself. On Linux, passwords a browser keeps in your desktop's keyring
  (GNOME Keyring, KWallet) can't be read. The summary after the import
  tells you how many passwords stayed behind.

## Importing again

Importing the same profile again is harmless: a profile you already
imported shows as **Imported already**, and its space is left as it is.
