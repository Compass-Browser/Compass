# Widevine

Compass plays DRM-protected media (Netflix, Spotify, Prime Video, Disney+,
Apple Music on the web...) through Google's Widevine CDM, as Chromium does.

## How it works

- `enable_widevine = true` in `config/args/common.gn`. Chromium then builds
  the Widevine key system and the component installer for the library CDM.
- The CDM itself is not in the app bundle (Google only allows that for
  Chrome-branded builds). On first run the component updater downloads it
  from Google into `<user data>/WidevineCdm/<version>/` (a minute or two after
  launch, then it updates itself). `chrome://components` shows
  "Widevine Content Decryption Module" and its version.
- Copying a `WidevineCdm` folder from another Chromium browser (Arc, Dia,
  Chrome) into the Compass user data folder also works: the component
  installer registers an installed copy at startup.

## Limits: no VMP

Chrome is signed for Widevine's Verified Media Path (VMP): its framework has a
`.sig` file made with a Google certificate (CDM host verification,
`enable_cdm_host_verification`, Chrome-branded builds only). Compass has no
such signature, so the CDM runs as an unverified host:

- Sites that only need Widevine L3 play (most music services, many video
  sites).
- Services that require VMP refuse the licence, or cap the resolution (Netflix
  has historically refused; others fall back to SD).

Getting VMP needs a production certificate. Two routes:

1. Google's Widevine agreement (what Brave, Vivaldi, Opera have).
2. castlabs EVS (free, made for Electron but signs any app with the Widevine
   VMP format): sign the app after `codesign`, before notarization, in
   `tools/compass sign`. Open item: try it on a Release build.
