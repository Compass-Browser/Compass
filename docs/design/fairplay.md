# FairPlay and native HLS on macOS

On macOS, Compass plays native HLS (`<video src="….m3u8">`) with
AVFoundation's AVPlayer instead of Chromium's HLS demuxer, as Safari does.
This is the groundwork for FairPlay Streaming (`com.apple.fps`), which only
works through AVFoundation.

Status: milestone 1 (clear HLS through AVPlayer, shown in the page) is in.
FairPlay EME, media element polish, the "Present as Safari" site setting and
the sandbox are still to come (see Plan).

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
- **AVFPlayerFactoryImpl** (`media/avf/browser/`, bound per document by
  `patches/chrome-browser-chrome_browser_interface_binders.cc.patch`)
  launches one AVFoundation utility process for all players, passes the
  user agent and the page's origin as referrer, and gives each player a new
  unguessable plane id, registered with the GPU process together with the
  player's CoreAnimation context id. Renderers only ever see the plane id, so
  a compromised renderer can't make the GPU process host another context.
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

## Known gaps (milestone 1)

- **Only in CoreAnimation overlay mode.** When viz composites the frame
  itself (e.g. some filters, too many layers, or a DevTools/CDP screenshot),
  the video hole draws nothing.
- **Network outside Chromium.** AVPlayer fetches the playlists and segments
  with its own `NSURLSession`: no cookies yet, and the ad blocker, DevTools,
  service workers and Chromium's proxy settings don't see the requests.
- **Not sandboxed** (`kNoSandbox`, like the macOS notification service):
  AVPlayer needs network, audio and the FairPlay system services.
- No audio/text track selection; HLS subtitles render inside AVPlayer only if
  at all. HLS URLs without `.m3u8` (detected by content sniffing) still use
  Chromium's HLS demuxer.

## Plan

1. ~~Clear HLS through AVPlayer, visible in the page.~~
2. Media element integration: cookies, seeking edge cases, tracks,
   picture-in-picture and fullscreen, the sniffed-HLS fallback.
3. FairPlay EME: `com.apple.fps` with `skd` init data only (so hls.js, Shaka
   and video.js, which ask for `cenc`/`sinf` on MSE, keep using Widevine),
   an `AVContentKeySession` CDM in the utility process, `encrypted` events
   from AVPlayer's key requests.
4. "Present as Safari": a per-site setting (page info) that sends a Safari
   user agent without client hints.
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

- `compass_unittests`: `AVFFeaturesTest.*`, `ExternalLayerRegistryTest.*`.
