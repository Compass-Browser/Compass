# PlayReady

On Windows, Compass offers sites Microsoft's PlayReady DRM in addition to
Widevine (docs/design/widevine.md), as Edge and Chrome do.

## How it works

- Chromium already has the code: the key system
  (`components/cdm/renderer/playready_key_system_info.cc`), its registration
  (`chrome/common/media/cdm_registration.cc`, `AddPlayReady`) and the Media
  Foundation CDM that runs it (`media/cdm/win/`, in the Media Foundation
  service process).
- Upstream turns it on only for Chrome-branded builds
  (`enable_playready = is_win && is_chrome_branded` in
  `components/cdm/common/playready.gni`).
  `patches/components-cdm-common-playready.gni.patch` makes it `is_win`.
  An arg in `config/args/` can't do this: args files can't see `target_os`,
  which the Windows cross build sets after the imports, and the
  Windows-to-Linux build would hit the gni's `is_win` assert.
- Unlike Widevine, nothing gets downloaded or signed. The CDM is part of
  Windows (`Windows.Media.Protection.PlayReady.dll`), and the protected path
  runs through the OS and the GPU. VMP doesn't apply.

## What sites get

- Only hardware-secure PlayReady (SL3000):
  `com.microsoft.playready.recommendation` and
  `com.microsoft.playready.recommendation.3000`. Upstream doesn't register
  software PlayReady (SL2000).
- Only on Windows 11, with a GPU and driver that support Media Foundation
  encrypted playback (`media::SupportMediaFoundationEncryptedPlayback()`).
  On anything else the key system isn't registered, and sites fall back to
  Widevine.
- Not in incognito windows: the Media Foundation CDM writes data to disk
  (`AddPlayReady` in `components/cdm/renderer/key_system_support_update.cc`).
- After crashes or repeated failures, Chromium can turn hardware-secure
  decryption off for a while (`kHardwareSecureDecryptionFallback`), and
  sites then get Widevine alone.

## Limits

- Sites pick the DRM system. Most ask for Widevine first. Some use PlayReady
  only for browsers they recognise (Netflix uses it for Edge's user agent),
  and a licence server can refuse an unfamiliar client. PlayReady helps
  only where a site asks for it and its licence server accepts Compass.
- PlayReady exists only on Windows. On Android, Chromium supports only
  Widevine (`MediaDrmBridge`), and few phones have PlayReady. macOS and Linux
  have none.

## Checking it

- `chrome://media-internals` shows the CDM in use during playback.
- In DevTools:

  ```js
  await navigator.requestMediaKeySystemAccess(
      'com.microsoft.playready.recommendation.3000',
      [{videoCapabilities: [{contentType: 'video/mp4; codecs="avc1.42E01E"'}]}])
  ```

  This resolves on a supported Windows 11 machine and rejects with
  `NotSupportedError` elsewhere.
- Microsoft's test page plays SL3000 content:
  https://testweb.playready.microsoft.com/
