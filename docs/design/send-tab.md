# Send a tab to the phone

Status: in progress (2026-11-02). iOS receiving part on `ios/send-tab`;
desktop and server parts handed off (below).

## Goal

On the desktop, "Send to your devices" on a tab (Chromium's send tab to
self) lists the iPhone; picking it shows a notification on the phone,
"Tab from MacBook", with the page's title and address. A tap opens the tab
in Compass and the desktop stops offering it.

## Data

Chromium's own `SEND_TAB_TO_SELF` type (EntitySpecifics field 601980),
unchanged, so the desktop needs no new format:

- The phone's `DEVICE_INFO` sets `feature_fields.send_tab_to_self_receiving_enabled
  = true` and `send_tab_to_self_receiving_type = CHROME_AND_PUSH_NOTIFICATION`,
  which puts it in the desktop's target list.
- The desktop writes an entry: `guid` (also the client tag), `url`, `title`,
  `device_name` (the sender), `target_device_sync_cache_guid` (the phone's
  cache GUID in that account), `shared_time_usec`.
- The phone marks it `opened` (and `notification_dismissed`) when the
  notification is tapped. Entries older than 10 days (Chromium's
  `kExpiryTime`) aren't shown.
- Like every type, it's encrypted with the chain's passphrase (sync.md): the
  server sees that a SEND_TAB_TO_SELF entity was committed, not its target
  or URL.

## Delivery

1. **Without push (done on iOS).** The phone gets the entry in its next
   sync cycle: at once while Compass is open (the SSE stream,
   `/v2/compass/events`), else at the next background refresh (iOS decides,
   about every 15 minutes at best) or launch. It posts a local notification.
2. **With push (to do).** The server wakes the phone through APNs as soon as
   the entry is committed:
   - **Phone → server**: after `registerForRemoteNotifications`, the phone
     sends its token, per account, with the account's sync bearer token:
     `POST /v2/compass/push?client_id=<cache GUID>` with JSON
     `{"token": "<hex>", "environment": "sandbox" | "production", "topic":
     "it.cipi1965.compass"}`; `DELETE` the same URL on leaving the chain.
     Debug builds are `sandbox`, TestFlight and App Store `production`.
   - **Server**: when a commit adds a new SEND_TAB_TO_SELF entity (version
     0 → 1) in an account, it sends to every registered token of that
     account except the committer's an APNs alert push (token-based auth,
     `.p8` key; `apns-push-type: alert`, `apns-priority: 10`, `apns-topic`
     from the registration):
     `{"aps": {"alert": {"title": "New tab", "body": "Open Compass to see it"},
     "mutable-content": 1, "sound": "default", "thread-id": "sent-tab"},
     "compass": {"type": "send-tab"}}`. A token APNs answers 410 (or 400
     BadDeviceToken) for is dropped. The server can't tell which device the
     tab is for, so every other iPhone of the account is woken.
   - **Phone**: a notification service extension rewrites it before it
     shows: it runs one GetUpdates for SEND_TAB_TO_SELF with the account's
     keys (shared with the extension through the App Group keychain),
     decrypts the entry and sets the title ("Tab from MacBook"), body (title,
     host) and `userInfo` the app's own notifications use. If the entry isn't
     for this phone, it says "A tab was sent to another device" (dropping a
     notification needs Apple's filtering entitlement). The app's local
     notification for a GUID already shown by push is skipped.

## Parts

| Part | Who | State |
|---|---|---|
| iOS: DEVICE_INFO flag, `SendTabBridge`, local notifications, tap opens and marks opened | compass-b2 (iOS session) | written, `ios/send-tab` |
| iOS: APNs registration, token upload, notification service extension | compass-b2 | after the server part |
| Desktop: enable SEND_TAB_TO_SELF and its entry points | handed off | to do |
| Server: `/v2/compass/push`, APNs on new SEND_TAB_TO_SELF | compass-0d | done (`server/push`); live on the sync server without a key |
| Apple: an APNs auth key (Certificates, Identifiers & Profiles → Keys → Apple Push Notifications service), its key ID and the team ID, on the server | Matteo | to do |

### Desktop

sync.md lists `SEND_TAB_TO_SELF` among the excluded types ("needs FCM"). It
only needs FCM for Chromium's own push to Android/iOS Chrome; between
Compass devices the SSE notifier already brings new entries. To do:

- Sync it (remove it from the excluded types).
- The entry points Chromium has: the tab's context menu "Send to your
  devices", the page's context menu, and the share menu. Check they show
  with the custom server (they require an active SEND_TAB_TO_SELF type and
  a target device that isn't this one; the phone is one once its
  DEVICE_INFO has the flag).
- Receiving on the desktop (a tab sent to a Mac) is Chromium's own UI; not
  needed for this feature.
- Check: sending from the desktop to the phone puts an entry with the
  phone's cache GUID (`chrome://sync-internals`), and the phone marks it
  opened.

### Server

As in "With push" above, plus: a config for the APNs key (path, key ID,
team ID), sandbox and production hosts by registration, and an e2e test
with a fake APNs endpoint (the push is sent once per commit that adds an
entry, not for the phone's own "opened" update).
