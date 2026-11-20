---
title: Privacy
description: What Compass turns off compared with Chrome, what it still connects to, and why.
sidebar:
  order: 18
---

Compass is built on Chromium, the open-source project behind Chrome, but it isn't Chrome: it has no Google account sign-in and none of Google's built-in services. On top of that, Compass switches off several things that would otherwise contact Google in the background.

This page is about Compass on Mac, Windows and Linux. The full privacy policy, for every platform, is at [compassbrowser.org/privacy](https://compassbrowser.org/privacy). On iPhone, iPad and Android, **Settings** has a **Privacy Policy** link too.

## What's off in Compass

These never run in Compass:

- **Usage statistics and crash reports.** Compass sends no usage metrics, and crash reports stay on your computer.
- **Experiments.** Chrome's server-controlled feature trials and field tests aren't used.
- **Google account sign-in to the browser.** You can still sign in to Google websites in a tab as usual. Compass's own [sync](/sync/turn-on/) doesn't use a Google account.
- **Surveys and feedback upload.**
- **Gemini in the browser and Google's "AI Mode" checks.**
- **Google's translation and spelling services, and Google's page hints and models.**
- **Privacy Sandbox** ad-related APIs.

Compass also turns these off by default. You can turn the first two back on in **Settings** if you want them:

- **Password leak checks**, which send hashed parts of your saved passwords to Google.
- **Suggestions on error pages** ("did you mean…") and the connection probes that come with them. Hints about Wi-Fi networks that need you to sign in go with them.
- **Make searches and browsing better** (sending the pages you visit to your search engine) is never turned on for you.

And it removes a few background requests entirely: the network time check, autofill's server predictions (autofill still works, using what's on your computer), and the check for your Google accounts at every launch (it's made only when a Google sign-in cookie changes).

There's also no first-run screen and no profile picker at startup: Compass opens straight to your spaces.

## What Compass still connects to

Some connections are kept on purpose because things would break without them:

| What | Why |
|---|---|
| The Chrome Web Store and extension updates | To install [extensions](/guide/extensions/) and keep them up to date with security fixes |
| Web push | So sites and extensions you allowed can send notifications while their tab is closed. It starts only once one of them asks |
| Safe Browsing | Chromium's phishing and malware list updates |
| Security components | Certificate revocation lists, Certificate Transparency logs and similar security data Chromium keeps current |
| Your search engine | Google by default, with suggestions as you type in the address bar. You can pick any search engine in **Settings** |

Compass itself also connects to:

- **Its update server**, to check for new versions on Mac and Windows. The check doesn't include anything about you or your computer. See [Updates](/guide/updates/).
- **Your sync server**, if you turn on sync. Everything you sync is end-to-end encrypted on your devices before it leaves them: the server can't read it. See [Encryption](/sync/encryption/).
- **Your AI provider**, only if you set up [Claude](/guide/claude/), and only with what that page describes.

## Clean links

When you copy a page's link (with <kbd>⌘</kbd> <kbd>⇧</kbd> <kbd>C</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>C</kbd>, the command bar's **Copy URL**, or the sidebar's **Copy Link**), Compass leaves out parameters that only track where a link was shared or clicked. To keep them, turn off **Remove tracking from copied links** in **Settings** → **Privacy and security** → **Third-party cookies**, under **Send a "Do Not Track" request**.
