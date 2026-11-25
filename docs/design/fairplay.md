# FairPlay and native HLS on macOS

On macOS, Compass plays native HLS (`<video src="….m3u8">`) with
AVFoundation's AVPlayer instead of Chromium's HLS demuxer, as Safari does.
This is the groundwork for FairPlay Streaming (`com.apple.fps`), which only
works through AVFoundation.

Status: milestones 1–6 are in: native HLS (clear, VOD and live) plays
through AVPlayer, shown in the page, with the page's cookies and user agent;
FairPlay works through EME as in Safari, for native HLS and for MSE; sites
the user picks can be told Compass is Safari; the AVFoundation process is
sandboxed, and Chromium's HLS player takes over if it keeps crashing.

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

So Safari's way of doing FairPlay on MSE (what hls.js, Shaka, video.js and
the big services' own players use) isn't open to Compass. Compass gets there
another way: it turns the MSE stream into HLS for AVPlayer ("FairPlay for
MSE" below). Native HLS is the form most FairPlay players take on the web
anyway (Bitmovin, Brightcove, JW Player, Radiant use it only that way, in
Safari).

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
  process's main thread, which gets a run loop (`--message-loop-type-ui`;
  sandboxed, an NSRunLoop rather than NSApplication). Each `AVPlayerLayer`
  is the root of its own `CAContext` and keeps a fixed size
  (`external_layer_constants.h`).
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
- **What's offered.** The playlist's types (`application/vnd.apple.mpegurl`
  and the like) with `skd` init data, for native HLS; and, with FairPlay for
  MSE, MP4 with H.264, HEVC or AAC and `cenc` init data
  (`FairPlayKeySystemInfo`). Without FairPlay for MSE (its feature off), MP4
  is offered only to pages presenting as Safari: an MSE player that picked
  `com.apple.fps` would send its samples to Chromium's decryptor, which
  can't use this CDM (Apple TV+ did, and failed: "CdmContextRef cannot be
  obtained").
- **Key system names.** Like Safari (`CDMFactoryFairPlayStreaming::
  supportsKeySystem()`), `com.apple.fps` and any `com.apple.fps.*`
  (`com.apple.fps.1_0` … `3_0`, which Bitmovin asks for); the CDM is made
  for `com.apple.fps`.

## FairPlay for MSE

MSE players (Shaka, hls.js, dash.js, Apple TV+'s and other services' own
players) append fMP4 to SourceBuffers and do EME with `cenc` init data.
Their samples can't be decrypted outside AVFoundation, and AVFoundation
takes FairPlay keys only from AVPlayer playing HLS. So Compass gives AVPlayer
the MSE stream as HLS: a playlist of fMP4 segments, rebuilt from the
demuxer's samples (still encrypted) and served on 127.0.0.1.

```
renderer: ChunkDemuxer --DecoderBuffers--> AVFMseRenderer
   --AVFMseSource.Append--> AVFoundation process: HlsSession (fMP4 + playlist)
   <--HTTP 127.0.0.1-- AVPlayer --skd://<key ID>--> AVContentKeySession
   --> AVFCdm --SPC--> page --license--> CKC
```

- **Choosing the renderer.** The pipeline waits for the CDM before it makes
  the renderer for an encrypted stream. When the CDM is FairPlay's, it asks
  for the embedder's renderer (`media::CdmRequiresEmbedderRenderer()`,
  `patches/media-base-cdm_context.*.patch`,
  `patches/media-base-pipeline_impl.cc.patch`, as Windows does for Media
  Foundation's CDM). `AVFCdmFactory` registers its CDMs' ids for that, and
  `AddRendererFactory()` (`media_factory.cc`'s patch) gives every player the
  AVFoundation factory, which makes an `AVFMseRenderer` for anything but
  native HLS.
- **`AVFMseRenderer`** (renderer, media thread) reads the demuxer streams
  and sends the samples (`AVFSample`: data, times, keyframe, the
  `DecryptConfig`) to the utility process, 32 at a time, up to 20 s ahead of
  AVPlayer. Each `StartPlayingFrom()` is a session (`AVFMseSource.
  StartSession`): AVPlayer gets a new item, which the service seeks to the
  start before replying. The picture is AVPlayer's layer through a video
  hole, as for native HLS; times come back as media times.
- **`HlsSession`** (`service/mse/`) undoes what Chromium's MP4 parser did:
  H.264/HEVC from Annex B back to length-prefixed NAL units, with the
  parameter sets it inserted moved into a rebuilt `avcC`/`hvcC`; AAC without
  its ADTS header. Start codes are only looked for in clear bytes, and the
  subsamples follow every change; encrypted bytes are never touched. It cuts
  segments at video keyframes at least 2 s apart (or where the parameter
  sets or the config change) once audio has caught up, muxing both tracks in
  each segment. Decode times are the presentation times sorted (decoder
  buffers carry none), with signed composition offsets (`trun` version 1).
  The init segment has `cbcs` protection (`encv`, `sinf`, `tenc` with the
  key ID, the pattern and the constant IV, or per-sample IVs if they vary),
  and a new one comes when any of that changes. The playlist is an `EVENT`
  playlist, `EXT-X-KEY` `skd://<key ID in hex>`, ending with `ENDLIST` once
  both streams ended. AVPlayer starts at its live edge whatever
  `EXT-X-START` says, so the service seeks it.
- **`LoopbackServer`** (`mse/loopback_server.mm`, Network.framework): HTTP
  on 127.0.0.1 and a random port, each session under a random path. The
  segments are as encrypted as the page's were. The sandbox allows binding
  and accepting on localhost.
- **The CDM.** `cenc` init data (PSSH boxes) gives a session its key IDs; a
  session whose init data names none takes the first key AVPlayer asks for.
  AVPlayer asks for `skd://<key ID>`; the SPC's content identifier is the
  raw key ID, as Safari's for MSE. Axinom's server accepts that. MSE
  players' assets are recipients of the CDM's own `AVContentKeySession`, so
  keys outlive players (a seek or a suspend needs no new license), and keys
  the CDM loaded before any media are found by AVPlayer.
- **Late MediaKeys.** The pipeline picks the renderer when it makes one; a
  player already playing with Chromium's renderer when FairPlay's CDM
  arrives (clear content first, MediaKeys set late) restarts its pipeline
  (`WebMediaPlayerImpl::ScheduleRestart()`,
  `patches/third_party-blink-renderer-platform-media-web_media_player_impl.cc.patch`).
- **Clear lead.** Content that starts clear and turns encrypted gets a new
  init segment and key after an `EXT-X-DISCONTINUITY`.
- **Limits.** `cbcs` only (FairPlay is `cbcs`; `cenc` content fails with an
  error); H.264, HEVC and AAC (no Dolby Vision, E-AC-3, VP9 or Opus); one
  audio and one video stream. A GOP longer than 10 s makes a segment longer
  than the `TARGETDURATION`. When Chromium suspends an idle paused player,
  the next play makes a new player, which finds the keys in the CDM's key
  session.
- Feature `CompassFairPlayMse` (on, on macOS; needs `CompassFairPlay`).
- **Checked:** Axinom's FairPlay test vector through MSE (its CMAF
  segments appended by hand, EME from the `encrypted` event): SPC, license,
  `usable`, playing at 1280×720; seeks forward and back; a 75 s pause.
  Apple TV+ not yet.

## Safari's FairPlay protocol

Apple's own player (Apple TV+, hls.js everywhere, Safari included) doesn't
use plain `cenc`: it speaks Safari's protocol over EME (`fps_protocol.h`),
and waits for the SPCs before it loads any media.

- **Init data:** a `pssh` with FairPlay's system ID whose data is a box tree,
  `fpsd` { `fpsi` (scheme), `fpsk` per key { `fkri` (key ID), `fkai` (asset
  ID), `fkcx`, `fkvl` (protocol versions) } }. Messages are JSON,
  `[{"keyID": base64, "payload": base64}]`, the payload the SPC (from the
  CDM) or the CKC (from the page); `update("renew")` asks for renewals. An
  older hls.js format (binary `kreq`/`spcs`/`ckcs`) works too.
- **The CDM requests the keys itself** (`processContentKeyRequestWithIdentifier`
  on its key session, by `skd://<key ID>`) as soon as the session is made,
  with the key's protocol versions (`AVContentKeyRequestProtocolVersionsKey`)
  and its asset ID (else the key ID) as content identifier. MSE players find
  the keys loaded. A request made from the page's `pssh` (as WebKit does) is
  named by the raw key ID instead, and AVPlayer then asks for the key again
  by its `skd://` URI, a second license pages don't give; a CKC can't answer
  two SPCs (-12158); naming the key by a FairPlay `pssh` in the playlist
  (`KEYFORMAT="urn:uuid:94ce86fb…"`) makes AVPlayer skip decryption.
- **Widevine** isn't offered on pages presenting as Safari: Apple's player
  ranks FairPlay < PlayReady < Widevine and takes Widevine whenever it's
  there.
- **Checked** locally against Axinom's license server with a page speaking
  either format the way Apple's hls.js does (`fpsd` init data, one license
  per key, licensed before any media): SPC, CKC, `usable`, MSE playing.
- **Apple TV+ (2026-11-25):** with Present as Safari, Apple's player takes
  FairPlay and the 4K HEVC stream, the CDM makes its SPCs, AVPlayer plays
  the clear lead at 1920×1080 and 3840×1600 unobscured, but Apple's license
  server answers `status -1002` (once `-1021`/`-42590`, before protocol
  versions were sent). The same title didn't play in Safari for the same
  account either, so that may be the account or the title, not Compass:
  untried with a title Safari plays. Native HLS previews report
  `outputObscuredDueToInsufficientExternalProtection` (HDCP) at 480×270.

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
  `com.apple.fps` also accepts `"cenc"` init data and MP4, as Safari's
  does, because players probe it so. With FairPlay for MSE every page gets
  that.
- **Limits:** only the user agent changes. `navigator.vendor`,
  `window.chrome`, `navigator.userAgentData` (present, with no brands) and
  the engine itself still differ from Safari, so a site that looks further
  may still tell; Cloudflare challenged bitmovin.com once on first visit.

### Legacy API

Safari still has its older, prefixed EME, and some players use FairPlay
only through it when it's there (Bitmovin: `window.WebKitMediaKeys ?` the
legacy path `:` the standard one). Pages presenting as Safari get it too:
`media/avf/renderer/legacy_eme.js`, built on the standard `com.apple.fps`
above.

- **Where:** `MaybeInjectLegacyEme()` (`legacy_eme.{h,cc}`), called from
  `ChromeContentRendererClient::RunScriptsAtDocumentStart()`
  (`patches/chrome-renderer-chrome_content_renderer_client.cc.patch`), runs
  the script in the main world before the page's own, if FairPlay is on and
  the frame's navigation committed with Safari's user agent
  (`IsSafariUserAgent()`, shared with `IsPresentingAsSafari`). Embedded
  with `build/embed_text.py`.
- **What it adds**, as WebKit's `CDMSessionAVFoundationObjC`:
  `WebKitMediaKeys` (`com.apple.fps` and `com.apple.fps.*`;
  `isTypeSupported()`; `createSession(type, initData)`),
  `WebKitMediaKeySession` (`update()`, `close()`, `sessionId`, `error`,
  events `webkitkeymessage`, `webkitkeyadded`, `webkitkeyerror` and their
  `on…` properties), `WebKitMediaKeyError`, and on media elements
  `webkitSetMediaKeys()`, `webkitKeys` and `webkitneedkey`.
- **How it maps:** an `encrypted` event with `skd` init data (a capturing
  listener on `window`, so elements in the document) is also dispatched as
  `webkitneedkey`, with Safari's init data: a u32 LE length, then the URI in
  UTF-16. `createSession()`'s init data is that, then the content id and the
  application certificate, each after its length; the session is
  `setServerCertificate(certificate)` and `generateRequest('skd', URI)` on
  `MediaKeys` the `WebKitMediaKeys` made, which `webkitSetMediaKeys()` sets
  on the element. `message` becomes `webkitkeymessage` (`message`,
  `destinationURL` empty); `update(ckc)` is `update()`; a `usable` key is
  `webkitkeyadded`; `internal-error`, a refused update or bad init data are
  `webkitkeyerror` (`MEDIA_KEYERR_UNKNOWN` or `_CLIENT`). The page's
  originals (`requestMediaKeySystemAccess`, `setMediaKeys`,
  `addEventListener`) are kept at start, so the page's wrappers don't see it.
- **Differences from Safari:** the SPC's content identifier is the skd://
  URI (as Safari's standard EME), not the content id the page put in the
  init data; license servers that check it against something else would
  refuse. Detached media elements get no `webkitneedkey`. Both `encrypted`
  and `webkitneedkey` fire.
- **Checked:** Bitmovin 8.285 on bitmovin.com (Present as Safari on) with
  Axinom's FairPlay test vector: `webkitSetMediaKeys(com.apple.fps.1_0)`,
  `createSession`, SPC, CKC, `webkitkeyadded`, playing at 1280×720. Axinom
  wants the raw SPC and returns a raw CKC, so the source's `fairplay` config
  needs `prepareMessage: (e) => e.message`, `licenseResponseType:
  'arraybuffer'` and `prepareLicense: (l) => new Uint8Array(l)`; Bitmovin's
  default is `spc=<base64>` form data, which Axinom refuses (as it would in
  Safari).

## The sandbox

The AVFoundation process fetches and parses untrusted media, so it runs in
its own Seatbelt profile, `media/avf/service/avfoundation_media.sb`, as
sandbox type `kAVFoundationMedia` (a new value in `sandbox.mojom`, with the
hooks every macOS type has: `patches/sandbox-policy-*`,
`patches/content-browser-sandbox_parameters_mac.mm.patch`,
`patches/content-browser-service_host-utility_sandbox_delegate.cc.patch`).
On top of `common.sb`:

- CoreAnimation and WindowServer (the `CAContext`), VideoToolbox and
  IOSurface/IOAccelerator, as `gpu.sb`;
- CoreAudio output, as `audio.sb`;
- outbound network, DNS and TLS trust, as `network.sb` (AVPlayer fetches the
  stream itself);
- CoreMedia's services, and **`com.apple.lskdd.xpc`, FairPlay's key
  daemon** (without it no SPC: "The operation could not be completed");
- fonts (`allow-font-access`): AVPlayerLayer draws captions itself;
- the Darwin user cache and temp directories (`AddDarwinDirs()`).

Denied, and fine without: analytics and telemetry, TCC, usage tracking,
camera extensions, and CFNetwork's on-disk HTTP cache and cookie store for
the helper (`~/Library/Caches/<helper>/Cache.db`, `HTTPStorages`), so AVPlayer
keeps nothing on disk, in Incognito too.

Finding what it needs: blocked mach lookups don't show as `deny` lines but
as `failed to do a bootstrap look-up: xpc_error=[159]` in the process's own
log (`/usr/bin/log show --info --debug --predicate 'processID == <pid>'`;
zsh has a `log` builtin), with `--enable-sandbox-logging`.

## When the AVFoundation process crashes

The browser keeps one AVFoundation process and never shuts it down, so
losing it means it crashed; it's relaunched for the next player. After two
crashes in a session (`kMaxServiceCrashes`), `AVFPlayerFactory` creates no
more players or FairPlay CDMs, and players use Chromium's HLS player:

- `AVFDemuxer` answers its initialization with `DEMUXER_ERROR_DETECTED_HLS`
  (also when the process dies while loading) and marks AVFoundation
  unusable in the renderer process (`SetAVFoundationUnavailable()`), which
  turns the AVF renderer factory's condition off;
- `DemuxerManager` then restarts the player with its HLS demuxer, without
  the data source its own HLS fallback expects
  (`patches/media-filters-demuxer_manager.cc.patch`).

A crash during playback stops that player with an error.

## Known gaps

- **A video hole inside a nested render pass** (the video under a CSS filter
  or an opacity animation, for example) shows nothing when the frame isn't
  all CA layers: the underlay only works for the root pass. DevTools/CDP
  screenshots and `drawImage(video)` never show the video.
- **Network outside Chromium.** AVPlayer fetches the playlists and segments
  with its own `NSURLSession`: the ad blocker, DevTools, service workers and
  Chromium's proxy settings don't see the requests, and a third-party stream
  gets no cookies.
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
   CKC gives `internal-error`. Apple TV+ (tv.apple.com) uses MSE outside
   Safari: it played with Widevine at 864×432, then picked FairPlay once
   `com.apple.fps` offered MP4, and failed until FairPlay for MSE. Not yet:
   persistent sessions, HDCP policy
   checks (`getStatusForPolicy()` says `usable`; AVFoundation enforces the
   CKC's output rules itself).
4. ~~"Present as Safari"~~ (above), with Safari's legacy prefixed EME
   (`WebKitMediaKeys`) for the players that still use it there.
5. ~~The sandbox, the fallback when the process crashes~~ (above).
6. ~~FairPlay for MSE~~, with Safari's FairPlay protocol (above). Next:
   Apple TV+ with a title Safari plays, a fresh key request when a page asks
   again for a key whose earlier request failed, more codecs (E-AC-3, Dolby
   Vision), HDCP for native HLS.
   Then: tracks, DVR seeking, sniffed HLS.

## Checking it

- Open an HLS stream, e.g. Apple's test stream
  `https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_fmp4/master.m3u8`,
  in a `<video>`: Activity Monitor shows a "Compass Media (AVFoundation)"
  helper, and the video plays with Chromium's controls.
- `--disable-features=CompassAVFoundationHls` goes back to Chromium's HLS
  demuxer.

## Tests

- `compass_unittests`: `AVFFeaturesTest.*` (with `SafariUserAgent`),
  `ExternalLayerRegistryTest.*`, `PresentAsSafariTest.*`;
  `FpsProtocolTest.*` (both formats, Apple TV+'s captured init data),
  `SampleConversionTest.*` (Annex B back to length-prefixed NAL units, clear
  and encrypted, a fake start code in encrypted bytes, `avcC`/`hvcC` from
  x264's and x265's parameter sets, ADTS), `Mp4WriterTest.*` (init and media
  segments read back with Chromium's MP4 parser), `HlsSessionTest.*`
  (segments at keyframes, the playlist's map, key and end, a session
  starting between keyframes, a clear lead, audio only, VP9 and `cenc`
  refused).
- `compass_browsertests` (macOS): `AVFHlsBrowserTest.PlaysHls` (AVPlayer
  fetches the stream, `X-Playback-Session-Id`, and the page gets its
  duration and size), `SendsFirstPartyCookies`,
  `AVFHlsBrowserTest.FallsBackAfterCrashes` (the process killed twice:
  relaunched, then Chromium's HLS player),
  `AVFHlsDisabledBrowserTest.UsesChromiumHls`, `FairPlayBrowserTest.
  Configurations` (native HLS and MSE configurations work; VP9, `sinf` and
  persistent licenses don't), `PlaysMse` (a clear MSE stream with
  FairPlay's MediaKeys plays through AVPlayer to its end), `Sessions`
  (certificate first, skd:// URIs, a session waiting for its key),
  `FairPlayMseDisabledBrowserTest.NativeHlsOnly`,
  `FairPlayDisabledBrowserTest.NotOffered`.
  `PresentAsSafariBrowserTest.SafariForTheUsersSites` (the page, its
  requests and AVPlayer's get Safari's user agent; other sites and the
  switch off don't), `SafariFairPlayConfigs` (`cenc` and `com.apple.fps.N_0`
  on such a site and, with FairPlay for MSE, elsewhere), `SafariLegacyEme`
  (`WebKitMediaKeys` there and not elsewhere, `webkitneedkey`'s init data, a
  session waiting for its key, bad init data as `webkitkeyerror`).
  Playing protected content needs a license server, so it's checked by hand
  (Axinom's test vector). The fixture in
  `media/avf/test/data/` is 6 s of Tears of Steel (CC BY 3.0).
