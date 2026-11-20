---
title: Encryption and privacy
description: How Compass sync keeps your data private, explained simply. No account, and a server that can't read what it stores.
sidebar:
  order: 5
---

Compass sync is built so that the sync server never sees your data, whoever
runs it: the Compass project, your company, or you.

## No account

You don't sign up for Compass sync. There's no email address and no
password. Your devices form a **sync chain** instead, and what holds it
together is a secret only your devices know.

(A company's own sync server can ask you to sign in with your work account.
That only decides who may use the server; it doesn't change any of the
below. See [Company sign-in](/sync/company-sign-in/).)

## The secret your devices share

When you start sync, your device makes a random secret, the chain's **seed**.
Every device in the chain gets the same seed, and from it, each device
works out two keys:

- **A key that encrypts your data.** Everything is encrypted on your device
  before it's uploaded, and only devices with the seed can decrypt it.
- **A key that identifies your chain to the server**, so the server knows
  which data is yours without knowing who you are.

The seed is stored on each device, protected the same way as your saved
passwords.

## The recovery code

Your **recovery code** is the seed written as 12 ordinary words. It's how
you get back in if you lose every device: type the 12 words on a new device
and it joins your chain with all your data.

- Keep it somewhere safe and private, such as a password manager.
- Anyone who has it can join your chain and read your data.
- If you lose every device **and** the recovery code, your synced data is
  gone. Nobody can recover it for you, because nobody else has the seed.

## Adding a device without sending the secret

When you add a device with a pairing code, the code is never sent to the
server. Your two devices use it to set up a private, encrypted connection
through the server, and the seed travels over that. The server only passes
along messages it can't read.

A code allows a single guess: if someone types a wrong one, it stops working
and you get a new one. And the device already in your chain always asks
you to **Allow** the new device, by name, before it sends anything.

## What the server can see

The server stores your data as encrypted blobs. It **can't** see your
spaces, tabs, URLs, passwords, history, or the names of your profiles.

It **can** see:

- what kind of data each item is (for example, a password or a history
  entry), how big it is, and when it changed;
- the list of your chain's devices;
- the network address your devices connect from, as any server can.

Each profile syncs in an account of its own, with its own key, so even the
server can't mix up the data of your Work and Personal profiles.

## Removing a device

Every device in a chain holds the seed, so a device can't be locked out
remotely. If you lose a device you don't trust anymore, use **Reset sync**
on a device you still have: it makes a new chain with a new seed and a new
recovery code, and you add your other devices to it again. See
[Leave or reset sync](/sync/leave-or-reset/).

## Want the technical details?

The full design, with the exact cryptography, is in the
[sync design document](https://github.com/Compass-Browser/Compass/blob/main/docs/design/sync.md)
on GitHub.
