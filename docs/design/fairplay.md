# FairPlay and native HLS on macOS

On macOS, Compass plays native HLS (`<video src="….m3u8">`) with
AVFoundation's AVPlayer instead of Chromium's HLS demuxer, as Safari does.
This is the groundwork for FairPlay Streaming (`com.apple.fps`), which only
works through AVFoundation.

Status: milestones 1–4 are in: native HLS (clear, VOD and live) plays
through AVPlayer, shown in the page, with the page's cookies and user agent;
FairPlay works through EME as in Safari; sites the user picks can be told
Compass is Safari. The sandbox is still to come (see Plan).

## Why AVPlayer, and not a Chromium renderer

The Phase 0 spike (November 2026) tried every route a third-party app has:

- Attaching an `AVContentKey` to sample buffers Compass builds itself
  (`AVSampleBufferAttachContentKey`) needs the private entitlement
  `com.apple.private.coremedia.allow-fps-attachment`; macOS kills an app that
  claims it.
- Adding `AVSampleBufferDisplayLayer` as a content key recipient fails with
  "no content key present": key association needs Apple's private
  `AVStreamDataParser`.
- **AVPlayer with an `AVURLAsset` registered with the key session works**,
  from an HLS URL. `AVAssetResourceLoaderDelegate` can serve playlists but not
  media segments (-12881), so AVPlayer fetches the segments itself.

So FairPlay on MSE (what hls.js, Shaka and video.js would use) isn't possible
with public APIs, and native HLS is the form FairPlay takes on the web anyway
(Bitmovin, Brightcove, JW Player, Radiant use it only that way, in Safari).

## How it works

```
renderer process                         browser process
 <video src=*.m3u8>                       AVFPlayerFactoryImpl (per document)
 AVFDemuxer + AVFRenderer ──AVFPlayerFactory──► launches the utility process,
   paint a video-hole frame (plane id)          registers {plane id → CAContext id}
                         │                      with the GPU process
                         └──AVFPlayer / AVFPlayerClient──┐
                                                         ▼
                        AVFoundation utility process (AVFMediaService)
                          AVPlayer + AVURLAsset, AVPlayerLayer in a CAContext
GPU process
  VideoHoleDrawQuad(plane id) → CA overlay → kExternal content layer
    → CALayerHost(context id), scaled to the quad
```

- **Choosing AVPlayer.** `compass::avf::ShouldUseAVFoundation()` (an http(s)
  URL whose path ends in `.m3u8`, the same test as Chromium's
  `DemuxerManager::IsManifestDemuxerURL()`; feature `CompassAVFoundationHls`,
  on by default on macOS). `content/renderer/media/media_factory.cc` then uses
  `AVFDemuxer` as the demuxer override and adds `AVFRendererFactory` as a
  conditional renderer factory
  (`patches/content-renderer-media-media_factory.cc.patch`).
  `patches/media-filters-demuxer_manager.cc.patch` lets a demuxer override win
  over the HLS demuxer.
- **AVFDemuxer** (`media/avf/renderer/`) reads no media. It asks the browser
  for an AVPlayer, waits for its metadata, reports the duration and buffered
  ranges, and offers placeholder audio/video streams so the pipeline and
  `WebMediaPlayerImpl` know what there is and the video's size.
- **AVFRenderer** drives the AVPlayer (seek, rate, volume), extrapolates the
  media time between updates, maps AVPlayer's stalls to buffering states, and
  paints a video-hole frame (`VideoFrame::CreateVideoHoleFrame`) with the
  player's overlay plane id.
  - The pipeline destroys its renderer when it suspends (a paused or
    background player) and makes a new one on resume, while the AVPlayer
    lives on in the demuxer. A new renderer starts from the demuxer's copy of
    the AVPlayer's last state (enough data, time), since AVPlayer reports
    only changes; a renderer destroyed while playing pauses the AVPlayer.
  - **Live streams** (an infinite duration): not seekable, as with
    Chromium's HLS demuxer (`patches/media-filters-demuxer_manager.cc.patch`
    lets `IsLiveContent()` ask this demuxer), and the pipeline's start "from
    0" doesn't seek, so AVPlayer stays near the live edge.
- **AVFPlayerFactoryImpl** (`media/avf/browser/`, bound per document by
  `patches/chrome-browser-chrome_browser_interface_binders.cc.patch`)
  launches one AVFoundation utility process for all players, passes the
  user agent (the tab's override, if it has one) and the page's origin as
  referrer, and gives each player a new
  unguessable plane id, registered with the GPU process together with the
  player's CoreAnimation context id. Renderers only ever see the plane id, so
  a compromised renderer can't make the GPU process host another context.
  - **Cookies:** the profile's cookies for the stream's URL
    (`GetCookieList()`, the SameSite context of a subresource of the frame),
    when the stream is first-party to the frame and `CookieSettings` allows
    full cookie access there; AVPlayer sends them with every request
    (`AVURLAssetHTTPCookiesKey`). Cookies set by AVPlayer's responses don't
    come back to the profile.
- **AVFMediaService** (`media/avf/service/`, registered in
  `patches/chrome-utility-services.cc.patch`) runs AVPlayer on the utility
  process's main thread, which gets an NSApplication run loop
  (`--message-loop-type-ui`). Each `AVPlayerLayer` is the root of its own
  `CAContext` and keeps a fixed size (`external_layer_constants.h`).
- **GPU process.** `compass::ExternalLayerRegistry` (`ui/external_layer/`,
  bound by `patches/chrome-gpu-browser_exposed_gpu_interfaces.cc.patch`) maps
  plane ids to context ids. A video-hole quad becomes a CA overlay
  (`patches/components-viz-service-display-ca_layer_overlay.cc.patch`), its
  plane id rides on `OverlayCandidate` and `CARendererLayerParams`, and
  `CARendererLayerTree` gives it a `kExternal` content layer: a CALayer
  holding a `CALayerHost` for the context, scaled to the quad's rect
  (`patches/ui-accelerated_widget_mac-ca_renderer_layer_tree.mm.patch`). It
  sits in the page's own layer tree, so HTML drawn above the video (player
  controls) stays above it.
- **When the frame can't be all CA layers** (any quad CoreAnimation can't
  take, e.g. a rotating clipped layer, which Compass's own UI has), viz
  composites the frame itself. The video hole then goes beneath it as an
  underlay, like protected video
  (`CALayerOverlayProcessor::PutForcedOverlayContentIntoUnderlays()`), with a
  hole punched in the frame; a frame with a video hole gets an alpha format
  for that (`patches/components-viz-service-display-direct_renderer.cc.patch`).
  HTML above the video still draws above it.

## FairPlay through EME

Pages use FairPlay as they do in Safari:

```js
video.addEventListener('encrypted', async (e) => {  // e.initDataType: 'skd'
  const access = await navigator.requestMediaKeySystemAccess('com.apple.fps', [{
    initDataTypes: ['skd'],
    videoCapabilities: [{contentType: 'application/vnd.apple.mpegurl'}],
  }]);
  const keys = await access.createMediaKeys();
  await keys.setServerCertificate(certificate);  // the app certificate
  await video.setMediaKeys(keys);
  const session = keys.createSession();
  session.onmessage = async (m) =>                // m.message: the SPC
      session.update(await postToLicenseServer(m.message));  // the CKC
  await session.generateRequest(e.initDataType, e.initData);
});
video.src = 'https://…/master.m3u8';
```

- **Key requests.** Each AVPlayer has its own `AVContentKeySession`, with the
  asset as recipient, from before it loads. A key request (`skd://…` from
  the playlist's `EXT-X-KEY`) becomes the page's `encrypted` event, once per
  key: `AVFPlayerClient.OnEncrypted` → `AVFDemuxer` → a new
  `Demuxer::SetEncryptedMediaInitDataCB()` that `DemuxerManager` gives
  overrides (`patches/media-base-demuxer.h.patch`,
  `patches/media-filters-demuxer_manager.cc.patch`) → `WebMediaPlayerImpl`.
  The init data type is a new `EmeInitDataType::SKD`, `"skd"`
  (`patches/media-base-eme_constants.*`, Blink's sanitizer in
  `patches/third_party-blink-renderer-platform-media-web_content_decryption_module_session_impl.cc.patch`);
  its data is the URI in UTF-8, as in WebKit.
- **The key system.** `FairPlayKeySystemInfo` (`media/avf/renderer/`) is
  added to the renderer's key systems
  (`ChromeContentRendererClient::GetSupportedKeySystems()`,
  `patches/chrome-renderer-chrome_content_renderer_client.cc.patch`):
  `"skd"` init data only, temporary sessions, empty robustness, no
  distinctive identifier or persistent state, MP4 codecs and any scheme
  (AVFoundation decides what the stream is). Blink also accepts the playlist
  types (`application/vnd.apple.mpegurl`, without codecs) for it
  (`patches/third_party-blink-renderer-platform-media-key_system_config_selector.cc.patch`).
  **"skd" only is what keeps MSE players safe:** hls.js, Shaka and video.js
  ask for `com.apple.fps` with `"cenc"`/`"sinf"` and MSE; they get no
  configuration and keep using Widevine (otherwise they'd pick FairPlay over
  MSE, which can't work).
- **The CDM** (`AVFCdm`, `media/avf/service/`) lives in the AVFoundation
  utility process with the players and implements
  `media.mojom.ContentDecryptionModule`. The renderer reaches it with
  Chromium's own `media::MojoCdm`, made by `AVFCdmFactory`, which wraps the
  frame's CDM factory for `com.apple.fps`
  (`patches/content-renderer-media-media_factory.cc.patch`) and asks the
  browser (`AVFPlayerFactory.CreateCdm`) for one. Its `cdm_id` is what
  `AVFRenderer::SetCdm()` passes to `AVFPlayer.SetCdm`, which hands the
  player's key requests to that CDM.
- **Sessions.** `setServerCertificate()` is required before
  `generateRequest("skd", uri)`; the session takes the player's request for
  that URI (or waits for it), and
  `makeStreamingContentKeyRequestDataForApp` (certificate, content
  identifier = the URI) gives the SPC for the `message` event
  (`license-request`; renewals come as `license-renewal`). `update(ckc)`
  goes to `processContentKeyResponse`; AVFoundation's verdict becomes the
  key's status (`usable`, or `internal-error` and a decode error). A failed
  request that AVFoundation retries goes to the same session.
- **Loading before the key.** A FairPlay item becomes ready to play only once
  it has the key, which needs the CDM, which the pipeline attaches only once
  loading is done; so loading also finishes once the playlists give the
  duration, and the video's size comes later.
- Feature `CompassFairPlay` (on, on macOS; needs `CompassAVFoundationHls`).
- **Key system names.** Like Safari (`CDMFactoryFairPlayStreaming::
  supportsKeySystem()`), `com.apple.fps` and any `com.apple.fps.*`
  (`com.apple.fps.1_0` … `3_0`, which Bitmovin asks for); the CDM is made
  for `com.apple.fps`.

## Present as Safari

Many players use FairPlay only when they think they're in Safari (by user
agent), so even with FairPlay here they'd use Widevine. "Present as Safari"
is a per-site switch in the page info popup (macOS, with FairPlay on), off
for every site by default.

- **The sites:** `compass.present_as_safari.sites`, a profile list pref of
  registrable domains (`video.example.com` → `example.com`; IP addresses and
  non-http(s) URLs can't be added). `browser/present_as_safari/`.
- **The user agent:** a tab helper
  (`patches/chrome-browser-ui-tab_helpers.cc.patch`) gives main-frame
  navigations to those sites Safari 26's user agent
  (`kSafariUserAgent`) as the tab's user agent override, with no user agent
  metadata, so no client hints are sent (as in Safari); other sites get
  Compass's back. Subresources and AVPlayer's requests follow the page's
  user agent. Turning the switch on or off reloads the page.
- **FairPlay as in Safari:** on such pages (any page whose tab overrides the
  user agent with Safari's: `AVFPlayerFactory.IsPresentingAsSafari`),
  `com.apple.fps` also accepts `"cenc"` init data, as Safari's does,
  because players probe it so. Elsewhere it stays `"skd"`-only.
- **Limits:** only the user agent changes. `navigator.vendor`,
  `window.chrome`, `navigator.userAgentData` (present, with no brands) and
  the engine itself still differ from Safari, so a site that looks further
  may still tell; Cloudflare challenged bitmovin.com once on first visit.
  Players on Safari's legacy prefixed API (`WebKitMediaKeys`,
  `webkitneedkey`), which Chromium doesn't have, don't get FairPlay:
  Bitmovin 8.285 now picks FairPlay and native HLS here, and AVPlayer
  reports its key, but Bitmovin doesn't start a session; an implementation
  of the legacy API over this EME would be next.

## Known gaps

- **A video hole inside a nested render pass** (the video under a CSS filter
  or an opacity animation, for example) shows nothing when the frame isn't
  all CA layers: the underlay only works for the root pass. DevTools/CDP
  screenshots and `drawImage(video)` never show the video.
- **Network outside Chromium.** AVPlayer fetches the playlists and segments
  with its own `NSURLSession`: the ad blocker, DevTools, service workers and
  Chromium's proxy settings don't see the requests, and a third-party stream
  gets no cookies.
- **Not sandboxed** (`kNoSandbox`, like the macOS notification service):
  AVPlayer needs network, audio and the FairPlay system services.
- No audio/text track selection; HLS subtitles render inside AVPlayer only if
  at all. HLS URLs without `.m3u8` (detected by content sniffing) still use
  Chromium's HLS demuxer. No seeking in a live stream's DVR window.

## Plan

1. ~~Clear HLS through AVPlayer, visible in the page.~~
2. ~~Media element integration: cookies and the user agent, suspend and
   resume, live streams, the underlay when the frame isn't all CA layers.~~
   Checked: seeking (exact), rates, pause, ended, loop, picture-in-picture.
   Left for later: tracks, DVR seeking, sniffed HLS.
3. ~~FairPlay EME~~ (above). Checked against Axinom's FairPlay test vector
   with Apple's page pattern: SPC, CKC, `usable`, playing at 1280×720; a bad
   CKC gives `internal-error`. Not yet: persistent sessions, HDCP policy
   checks (`getStatusForPolicy()` says `usable`; AVFoundation enforces the
   CKC's output rules itself).
4. ~~"Present as Safari"~~ (above). Next: Safari's legacy prefixed EME
   (`WebKitMediaKeys`) for the players that still use it there.
5. A sandbox profile, falling back to Chromium's HLS demuxer if the service
   crashes, browser tests.

## Checking it

- Open an HLS stream, e.g. Apple's test stream
  `https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_fmp4/master.m3u8`,
  in a `<video>`: Activity Monitor shows a "Compass Media (AVFoundation)"
  helper, and the video plays with Chromium's controls.
- `--disable-features=CompassAVFoundationHls` goes back to Chromium's HLS
  demuxer.

## Tests

- `compass_unittests`: `AVFFeaturesTest.*`, `ExternalLayerRegistryTest.*`,
  `PresentAsSafariTest.*`.
- `compass_browsertests` (macOS): `AVFHlsBrowserTest.PlaysHls` (AVPlayer
  fetches the stream, `X-Playback-Session-Id`, and the page gets its
  duration and size), `SendsFirstPartyCookies`,
  `AVFHlsDisabledBrowserTest.UsesChromiumHls`, `FairPlayBrowserTest.
  Configurations` (Safari's configurations work, `cenc`/`sinf` and
  persistent licenses don't), `Sessions` (certificate first, skd:// URIs,
  a session waiting for its key), `FairPlayDisabledBrowserTest.NotOffered`.
  `PresentAsSafariBrowserTest.SafariForTheUsersSites` (the page, its
  requests and AVPlayer's get Safari's user agent; other sites and the
  switch off don't), `SafariFairPlayConfigs` (`cenc` and `com.apple.fps.N_0`
  on such a site, `skd` only elsewhere).
  Playing protected content needs a license server, so it's checked by hand
  (Axinom's test vector). The fixture in
  `media/avf/test/data/` is 6 s of Tears of Steel (CC BY 3.0).
