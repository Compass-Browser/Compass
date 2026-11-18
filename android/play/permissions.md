# Permissions Play asks about

The manifest is Chrome's (chrome/android/java/AndroidManifest.xml) plus
Compass's extras (android/AndroidManifest.xml). To list it from a build:
`bundletool dump manifest --bundle=Compass-<version>.aab` or `aapt2 dump
permissions Compass.apk`. These are the ones with a Play Console declaration
(App content → Sensitive app permissions, and the foreground service form),
and the answer for each. Chrome, Firefox and Edge ship the same set as
browsers.

| Permission | Why the browser has it | Play's declaration |
|---|---|---|
| `QUERY_ALL_PACKAGES` | Opening links in the app that handles them (intents), the share sheet, password autofill for other apps, payment apps | Permitted use: **web browser**. Say: "Compass is a web browser; it hands links to the apps that handle them and fills passwords in other apps." |
| `REQUEST_INSTALL_PACKAGES` | Opening an APK the user downloaded | Core use: **web browser** (the user downloads and opens files) |
| `FOREGROUND_SERVICE_DATA_SYNC` | Downloads that run on | Foreground service: data sync, user-initiated downloads. Video: start a large download, leave the app, the notification shows progress |
| `FOREGROUND_SERVICE_MEDIA_PLAYBACK` | Media from a web page playing on with the app in the background | Media playback. Video: play a video or audio page, leave the app, the media notification |
| `FOREGROUND_SERVICE_CAMERA`, `_MICROPHONE` | A web page using the camera or microphone (a video call) while the app is in the background | Camera and microphone, user-initiated by the site's permission prompt |
| `FOREGROUND_SERVICE_MEDIA_PROJECTION` | Not in Compass: Chromium shares the screen (`getDisplayMedia`) only on desktop Android, so android/AndroidManifest.xml sets `enable_screen_capture` to false and the permission and the `mediaProjection` service type go | None (the first bundle, 1263212351, still had it) |
| `FOREGROUND_SERVICE_SHORT_SERVICE` | Short work Chrome finishes after the app leaves the foreground | Short service: needs no declaration video |
| `ACCESS_FINE_LOCATION`, `ACCESS_COARSE_LOCATION` | Sites' geolocation, after the site's prompt; foreground only (no `ACCESS_BACKGROUND_LOCATION`) | No background location form. Data safety: open web, not declared |
| `CAMERA`, `RECORD_AUDIO` | Sites, and the QR code scanner for pairing | None beyond the runtime prompt |
| `READ_CONTACTS` | The web's Contact Picker API, at a site's request | None (runtime prompt) |
| `ACCESS_ADSERVICES_ATTRIBUTION` | Chromium's Attribution Reporting API through Android's Privacy Sandbox | Not the advertising ID: the app has no `com.google.android.gms.permission.AD_ID`, so "Does your app use advertising ID?" is **No** |

The declarations went in on 2026-11-18, each with a short video recorded in
an emulator (Android 16, arm64) with a new profile and no personal data,
from a local test page: in Google Drive, the folder "Compass for Android –
Play declarations" (anyone with the link), one file per declaration. To
record more, `adb shell screenrecord`; Play wants 90 seconds or less.
Picture-in-picture isn't declared: it didn't start in the emulator.

Autofill: Compass is an autofill service for other apps
(`BIND_AUTOFILL_SERVICE`, a permission the system holds, not the app). Play
has no form for it; the listing says passwords fill in other apps.
