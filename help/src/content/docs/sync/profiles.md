---
title: Profiles and sync
description: How Compass syncs several profiles, such as Personal and Work, in one sync chain.
sidebar:
  order: 4
---

In Compass, each space belongs to a [profile](/guide/profiles/), and each
profile keeps its own cookies, passwords and history. Sync keeps profiles
apart too: one sync chain carries **all** your profiles, and each profile
syncs in its own encrypted account on the server.

## What happens with several profiles

- Every profile that has spaces in your space switcher syncs. You don't
  need to turn sync on in each one.
- A profile's spaces, passwords and history only ever go to the same
  profile on your other devices. Work passwords never end up in Personal.
- A profile that exists on one device but not on another is created on the
  other device for you, with its name and picture.
- Renaming a profile renames it on your other devices too.
- Moving a space to another profile moves it on your other devices as well.
- Deleting a profile only deletes it on that device. Your other devices keep
  theirs.

A profile you create while sync is on starts syncing after you restart
Compass. The **Sync** page tells you when a restart is needed.

## Join with a computer that already has profiles

When you add a computer that already has more than one profile, Compass asks
how they line up with the chain's: it asks you to choose which profile of
your other devices each one syncs with.

1. For each profile on this computer, choose the chain's profile it should
   sync with, or let it join as a new profile. Compass suggests the one with
   the same name.
2. Click **Continue**.

The profile names follow the chain: if you map your "Job" profile to the
chain's "Work", it's renamed "Work" here. The chain's profiles that nothing
maps to are created on this computer.

## Switches are per profile

The switches under **What syncs** belong to the profile whose **Sync** page
you're looking at. To stop syncing history in your Work profile only, open
**Sync** from a window of a Work space and turn off **History**.

Some things are the same for the whole device, whichever profile's page you
use:

- **Leave sync** and **Reset sync** apply to every profile on the device.
- **Show recovery code** shows the same 12 words in every profile.
- **Add a device** adds the new device with all your profiles.

## On iPhone and iPad

Compass for iPhone and iPad syncs every profile of your chain and shows all
their spaces in one sidebar, in the same order as on your computer. When
your chain has more than one profile, each space shows a small label with
its profile's name.

- New tabs open in the current space, and new spaces go in the current
  space's profile.
- To move a space to another profile, open the space's menu and choose
  **Move to Profile**. Its tabs are signed out of sites, because each
  profile keeps its own cookies.
- The command bar only searches the current space's profile.
- In **Settings**, the **Profiles** section lists your profiles and how
  many spaces each one has.
