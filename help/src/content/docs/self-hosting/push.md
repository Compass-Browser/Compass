---
title: Push notifications
description: How sent tabs reach phones on your own server, and when the server can wake them with APNs or FCM.
sidebar:
  order: 7
---

When you send a tab to an iPhone or an Android phone ("Send to Your
Devices"), the phone shows it:

- **at once** while Compass is open on it, through the change-notification
  stream every device keeps open;
- otherwise **at its next sync**: when it's opened, or when the system lets
  it refresh in the background (on iPhone, iOS decides when, about every
  15 minutes at best).

A server can also wake the phone right away with a push notification
(Apple's push service on iPhone, Firebase Cloud Messaging on Android). The
tab itself never goes through Apple or Google: the push only says "new
tab", and the phone fetches and decrypts the tab from your server.

## Push needs the app's own keys

Apple and Google only accept pushes for an app from its developer:

- **APNs** needs an auth key of the Apple Developer team that publishes the
  app, for its bundle ID.
- **FCM** needs a service account of the app's Firebase project.

So a self-hosted server can't push to the Compass apps from the App Store
or Google Play: only the project's own server holds those keys. Sent tabs
still arrive as described above. Push on your server works only if you
build and sign Compass for iOS or Android yourself, under your own bundle
ID or Firebase project.

## Turn it on for your own build

**iPhone (APNs).** In the Apple Developer portal (Certificates, Identifiers
& Profiles → Keys), make a key with Apple Push Notifications service. Put
the `.p8` file where the container can read it and set:

```yaml
    volumes:
      - sync-data:/data
      - ./apns.p8:/run/secrets/apns.p8:ro
    environment:
      COMPASS_SYNC_APNS_KEY: /run/secrets/apns.p8
      COMPASS_SYNC_APNS_KEY_ID: ABC123DEFG      # the key's ID
      COMPASS_SYNC_APNS_TEAM_ID: XYZ987TEAM     # your team ID
      COMPASS_SYNC_APNS_TOPICS: com.example.compass   # your build's bundle ID
```

The file must be readable by the server's user (65532). Debug builds
register for Apple's sandbox, release builds for production; the server
uses the right one for each phone.

**Android (FCM).** In your build's Firebase project, make a service account
with the Firebase Cloud Messaging role, download its JSON key, mount it the
same way and set `COMPASS_SYNC_FCM_CREDENTIALS` to its path. Add your
package name to `COMPASS_SYNC_APNS_TOPICS` (the list covers Android package
names too).

Without a key, phones still register with the server and nothing is pushed.
The server stops pushing to a phone whose token Apple or Google reports as
gone.
