# Data safety (draft)

Answers for Play Console's App content → Data safety, from what the app
sends (docs/design/privacy.md, sync.md, android.md "Sync and push",
claude.md). A draft for the developer to check before submitting: the form
is the developer's legal statement.

Play's rules that decide most answers
(support.google.com/googleplay/android-developer/answer/10787469):

- Data that is **end-to-end encrypted**, unreadable by anyone but the sender
  and the recipient, isn't "collected".
- Data sent from **a webview where the user navigates the open web** needn't
  be declared, collected or shared. A browser's pages are that.
- A transfer the **user starts and expects** (here: asking Claude, with the
  user's own key and endpoint) isn't "sharing"; data the user sends to their
  own account at another service, which the app never collects, isn't
  "collection" either (Play's FAQ on uploads to the user's own cloud
  storage).
- Transfers to a **service provider** working for the developer (Firebase
  Cloud Messaging) aren't "sharing".

## What leaves the device

| Flow | Who receives it | Readable by the developer? | Declared as |
|---|---|---|---|
| Synced spaces, tabs, history, passwords, autofill, settings, open tabs, sent tabs | The sync server (the developer's, or the user's own) | No: encrypted on the device with a key only the user's devices have | Nothing (end-to-end encrypted) |
| Sync chain identifier (derived from the key), each item's type, size and time | The sync server | Yes | Device or other IDs |
| Device list entry: device name as Android reports it, model, Compass version, last sync time | The sync server, unencrypted, so other devices can list it | Yes | Device or other IDs |
| FCM registration token | The sync server (to wake the device for sent tabs and sync); Google's FCM delivers | Yes | Device or other IDs |
| Requests that fail: time, path, Compass version (no IP address) | Sync server logs, rotated | Yes | Covered by the above; App info and performance isn't used |
| Claude: the chat, the page's text, tool results | Anthropic's API or the endpoint the user set, with the user's key | No, never passes through the developer | Nothing (user-initiated, user's own account) |
| Pages, searches, site permissions (location, camera, microphone) | The websites and the search engine the user picks | No | Nothing (open web) |
| Extension and component updates, Chrome Web Store | Google, as in Chromium | No | Nothing: the browser's own components; no user data beyond the installed extensions' IDs and versions (see "Judgment calls") |
| Passwords filled into other apps, the QR code camera, the passwords' screen lock | Stay on the device | — | Nothing |

Not in the app: ads, analytics, crash report upload (dumps stay on the
device), Google sign-in, UMA/UKM metrics, an advertising ID.

## The form

**Data collection and security**

- Does your app collect or share any of the required user data types? **Yes**
  (only the device and sync identifiers above).
- Is all of the user data collected by your app encrypted in transit? **Yes**
  (HTTPS to the sync server; FCM is HTTPS).
- Which of the following methods of account creation does your app support?
  **My app does not allow users to create an account** (sync has no
  accounts: a chain of paired devices).
- Do you provide a way for users to request that their data is deleted?
  **Yes**: leaving sync on the last device, or asking the developer, removes
  the chain's data from the server. (No account deletion URL is needed
  without accounts.)

**Data types**

Only **Device or other IDs**:

- Collected: **Yes**. Shared: **No** (FCM is a service provider).
- Processed ephemerally: **No** (the server keeps them while the device
  syncs).
- Required or optional: **Users can choose whether this data is collected**
  (only with sync turned on; sync is off until the user pairs).
- Purposes: **App functionality** (sync, sending tabs between the user's
  devices, waking the device for them). Not analytics, advertising,
  personalization, fraud prevention or account management.

Every other type (location, personal info, financial info, health, messages,
photos and videos, audio, files and docs, calendar, contacts, app activity,
web browsing, app info and performance): **not collected, not shared**.
Web browsing history does sync, but end-to-end encrypted.

**Security practices to show:** data encrypted in transit; users can request
deletion. Don't claim an independent security review.

## Judgment calls to look at again

- **Chromium's Google services.** Extension and component updates and the
  Chrome Web Store send the browser's version and the installed extensions'
  IDs to Google, as every Chromium browser does; Safe Browsing (when it
  works without an API key, privacy.md) sends URL hash prefixes. They are
  treated here as the browser's own updates, not user data. A stricter
  reading declares **App activity → Installed apps** (shared with Google, for
  App functionality and Security).
- **Device list entries.** The device's name could be the user's name ("Ada's
  Pixel"). If that seems like personal info, add **Personal info → Name**,
  collected, optional, App functionality.
- **Privacy policy.** compassbrowser.org/privacy covers iPhone and iPad
  today. Before review it needs the Android app: the same sync section, FCM
  instead of APNs for push, Android's device name in the device list, Claude
  with the user's key, passwords in other apps (Android Autofill), and no
  TestFlight feedback (Play's instead).
