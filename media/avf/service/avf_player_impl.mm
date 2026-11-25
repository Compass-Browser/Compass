// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/avf_player_impl.h"

#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

#include <cmath>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/apple/foundation_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "compass/media/avf/service/avf_cdm.h"
#include "compass/media/avf/service/avf_mse_source_impl.h"
#include "compass/ui/external_layer/external_layer_constants.h"
#include "media/base/timestamp_constants.h"
#include "net/base/apple/url_conversions.h"
#include "ui/base/cocoa/remote_layer_api.h"

// Bridges KVO and notifications from AVFoundation to AVFPlayerImpl, on the
// main thread (where AVFPlayerImpl lives).
// The AVContentKeySession's delegate: hands FairPlay key requests to
// AVFPlayerImpl on the main thread (the session's delegate queue).
@interface CompassAVFKeyDelegate : NSObject <AVContentKeySessionDelegate>
- (instancetype)initWithOwner:(base::WeakPtr<compass::avf::AVFPlayerImpl>)owner;
@end

@implementation CompassAVFKeyDelegate {
  base::WeakPtr<compass::avf::AVFPlayerImpl> _owner;
}

- (instancetype)initWithOwner:
    (base::WeakPtr<compass::avf::AVFPlayerImpl>)owner {
  if ((self = [super init])) {
    _owner = owner;
  }
  return self;
}

- (void)contentKeySession:(AVContentKeySession*)session
    didProvideContentKeyRequest:(AVContentKeyRequest*)keyRequest {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequest(keyRequest, /*renewal=*/false);
  }
}

- (void)contentKeySession:(AVContentKeySession*)session
    didProvideRenewingContentKeyRequest:(AVContentKeyRequest*)keyRequest {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequest(keyRequest, /*renewal=*/true);
  }
}

- (void)contentKeySession:(AVContentKeySession*)session
    contentKeyRequestDidSucceed:(AVContentKeyRequest*)keyRequest {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequestSucceeded(keyRequest);
  }
}

- (void)contentKeySession:(AVContentKeySession*)session
           contentKeyRequest:(AVContentKeyRequest*)keyRequest
    didFailWithError:(NSError*)error {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequestFailed(keyRequest, error);
  }
}

- (BOOL)contentKeySession:(AVContentKeySession*)session
    shouldRetryContentKeyRequest:(AVContentKeyRequest*)keyRequest
                          reason:(AVContentKeyRequestRetryReason)retryReason {
  return NO;
}

@end

@interface CompassAVFObserver : NSObject
- (instancetype)initWithPlayer:(AVPlayer*)player
                          item:(AVPlayerItem*)item
                         owner:(base::WeakPtr<compass::avf::AVFPlayerImpl>)owner;
- (void)stop;
@end

namespace {

void* const kItemStatusContext = const_cast<void**>(&kItemStatusContext);
void* const kPresentationSizeContext =
    const_cast<void**>(&kPresentationSizeContext);
void* const kLoadedRangesContext = const_cast<void**>(&kLoadedRangesContext);
void* const kDurationContext = const_cast<void**>(&kDurationContext);
void* const kTimeControlContext = const_cast<void**>(&kTimeControlContext);
void* const kLikelyToKeepUpContext =
    const_cast<void**>(&kLikelyToKeepUpContext);

base::TimeDelta ToTimeDelta(CMTime time) {
  if (CMTIME_IS_INDEFINITE(time) || CMTIME_IS_POSITIVE_INFINITY(time)) {
    return media::kInfiniteDuration;
  }
  if (!CMTIME_IS_NUMERIC(time)) {
    return base::TimeDelta();
  }
  return base::Seconds(CMTimeGetSeconds(time));
}

}  // namespace

@implementation CompassAVFObserver {
  AVPlayer* __strong _player;
  AVPlayerItem* __strong _item;
  id __strong _timeObserver;
  NSMutableArray* __strong _notificationTokens;
  base::WeakPtr<compass::avf::AVFPlayerImpl> _owner;
  BOOL _stopped;
}

- (instancetype)initWithPlayer:(AVPlayer*)player
                          item:(AVPlayerItem*)item
                         owner:
                             (base::WeakPtr<compass::avf::AVFPlayerImpl>)owner {
  if ((self = [super init])) {
    _player = player;
    _item = item;
    _owner = owner;
    _notificationTokens = [NSMutableArray array];
    NSKeyValueObservingOptions options = NSKeyValueObservingOptionNew;
    [item addObserver:self
           forKeyPath:@"status"
              options:options
              context:kItemStatusContext];
    [item addObserver:self
           forKeyPath:@"presentationSize"
              options:options
              context:kPresentationSizeContext];
    [item addObserver:self
           forKeyPath:@"loadedTimeRanges"
              options:options
              context:kLoadedRangesContext];
    [item addObserver:self
           forKeyPath:@"duration"
              options:options
              context:kDurationContext];
    [item addObserver:self
           forKeyPath:@"playbackLikelyToKeepUp"
              options:options
              context:kLikelyToKeepUpContext];
    [player addObserver:self
             forKeyPath:@"timeControlStatus"
                options:options
                context:kTimeControlContext];

    __weak CompassAVFObserver* weakSelf = self;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [_notificationTokens
        addObject:[center
                      addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                  object:item
                                   queue:NSOperationQueue.mainQueue
                              usingBlock:^(NSNotification*) {
                                [weakSelf playedToEnd];
                              }]];
    [_notificationTokens
        addObject:
            [center
                addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification
                            object:item
                             queue:NSOperationQueue.mainQueue
                        usingBlock:^(NSNotification* notification) {
                          NSError* error = notification.userInfo
                              [AVPlayerItemFailedToPlayToEndTimeErrorKey];
                          [weakSelf failedToPlayToEnd:error];
                        }]];
    // Diagnostics (VLOG): rendition switches and playback errors.
    [_notificationTokens
        addObject:[center
                      addObserverForName:AVPlayerItemNewAccessLogEntryNotification
                                  object:item
                                   queue:NSOperationQueue.mainQueue
                              usingBlock:^(NSNotification*) {
                                [weakSelf accessLogChanged];
                              }]];
    [_notificationTokens
        addObject:[center
                      addObserverForName:AVPlayerItemNewErrorLogEntryNotification
                                  object:item
                                   queue:NSOperationQueue.mainQueue
                              usingBlock:^(NSNotification*) {
                                [weakSelf errorLogChanged];
                              }]];
    _timeObserver = [player
        addPeriodicTimeObserverForInterval:CMTimeMakeWithSeconds(0.25, 600)
                                     queue:dispatch_get_main_queue()
                                usingBlock:^(CMTime) {
                                  [weakSelf periodicTime];
                                }];
  }
  return self;
}

- (void)stop {
  if (_stopped) {
    return;
  }
  _stopped = YES;
  [_item removeObserver:self forKeyPath:@"status" context:kItemStatusContext];
  [_item removeObserver:self
             forKeyPath:@"presentationSize"
                context:kPresentationSizeContext];
  [_item removeObserver:self
             forKeyPath:@"loadedTimeRanges"
                context:kLoadedRangesContext];
  [_item removeObserver:self forKeyPath:@"duration" context:kDurationContext];
  [_item removeObserver:self
             forKeyPath:@"playbackLikelyToKeepUp"
                context:kLikelyToKeepUpContext];
  [_player removeObserver:self
               forKeyPath:@"timeControlStatus"
                  context:kTimeControlContext];
  for (id token in _notificationTokens) {
    [NSNotificationCenter.defaultCenter removeObserver:token];
  }
  [_notificationTokens removeAllObjects];
  if (_timeObserver) {
    [_player removeTimeObserver:_timeObserver];
    _timeObserver = nil;
  }
}

- (void)dealloc {
  [self stop];
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  // KVO can fire on any thread; AVFPlayerImpl lives on the main thread.
  if (![NSThread isMainThread]) {
    __weak CompassAVFObserver* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf dispatchContext:context];
    });
    return;
  }
  [self dispatchContext:context];
}

- (void)dispatchContext:(void*)context {
  compass::avf::AVFPlayerImpl* owner = _owner.get();
  if (!owner || _stopped) {
    return;
  }
  if (context == kItemStatusContext) {
    owner->OnItemStatusChanged();
  } else if (context == kPresentationSizeContext) {
    owner->OnPresentationSizeChanged();
  } else if (context == kLoadedRangesContext) {
    owner->OnLoadedRangesChanged();
  } else if (context == kDurationContext) {
    owner->OnDurationChanged();
  } else if (context == kTimeControlContext ||
             context == kLikelyToKeepUpContext) {
    owner->OnPlaybackStateChanged();
  }
}

- (void)playedToEnd {
  if (auto* owner = _owner.get()) {
    owner->OnPlayedToEnd();
  }
}

- (void)failedToPlayToEnd:(NSError*)error {
  if (auto* owner = _owner.get()) {
    owner->OnFailedToPlayToEnd(
        base::SysNSStringToUTF8(error.localizedDescription ?: @"unknown"));
  }
}

- (void)accessLogChanged {
  if (auto* owner = _owner.get()) {
    owner->OnAccessLogChanged();
  }
}

- (void)errorLogChanged {
  if (auto* owner = _owner.get()) {
    owner->OnErrorLogChanged();
  }
}

- (void)periodicTime {
  if (auto* owner = _owner.get()) {
    owner->OnPeriodicTime();
  }
}

@end

namespace compass::avf {

AVFPlayerImpl::AVFPlayerImpl(
    const GURL& url,
    mojom::AVFPlayerOptionsPtr options,
    mojo::PendingReceiver<mojom::AVFPlayer> receiver,
    mojo::PendingRemote<mojom::AVFPlayerClient> client,
    base::OnceClosure on_disconnect,
    FindCdmCallback find_cdm)
    : mse_(url.is_empty()),
      receiver_(this, std::move(receiver)),
      client_(std::move(client)),
      on_disconnect_(std::move(on_disconnect)),
      find_cdm_(std::move(find_cdm)) {
  receiver_.set_disconnect_handler(
      base::BindOnce(&AVFPlayerImpl::Disconnect, base::Unretained(this)));
  client_.set_disconnect_handler(
      base::BindOnce(&AVFPlayerImpl::Disconnect, base::Unretained(this)));

  NSMutableDictionary* asset_options = [NSMutableDictionary dictionary];
  if (!options->user_agent.empty()) {
    if (@available(macOS 13, *)) {
      asset_options[AVURLAssetHTTPUserAgentKey] =
          base::SysUTF8ToNSString(options->user_agent);
    }
  }
  if (options->referrer && options->referrer->is_valid()) {
    // Not in the SDK headers, but long supported and used widely.
    asset_options[@"AVURLAssetHTTPHeaderFieldsKey"] = @{
      @"Referer" : base::SysUTF8ToNSString(options->referrer->spec()),
    };
  }

  if (!options->cookies.empty()) {
    NSMutableArray<NSHTTPCookie*>* cookies = [NSMutableArray array];
    for (const auto& cookie : options->cookies) {
      NSMutableDictionary* properties = [NSMutableDictionary dictionary];
      properties[NSHTTPCookieName] = base::SysUTF8ToNSString(cookie->name);
      properties[NSHTTPCookieValue] = base::SysUTF8ToNSString(cookie->value);
      properties[NSHTTPCookieDomain] = base::SysUTF8ToNSString(cookie->domain);
      properties[NSHTTPCookiePath] =
          base::SysUTF8ToNSString(cookie->path.empty() ? "/" : cookie->path);
      if (cookie->secure) {
        properties[NSHTTPCookieSecure] = @"TRUE";
      }
      if (cookie->expiry) {
        properties[NSHTTPCookieExpires] = cookie->expiry->ToNSDate();
      }
      if (NSHTTPCookie* ns_cookie =
              [NSHTTPCookie cookieWithProperties:properties]) {
        [cookies addObject:ns_cookie];
      }
    }
    asset_options[AVURLAssetHTTPCookiesKey] = cookies;
  }

  // FairPlay: the asset's key requests come to this session before AVPlayer
  // loads anything, whether or not the page ever sets a CDM.
  key_requests_ = [NSMutableArray array];
  key_delegate_ =
      [[CompassAVFKeyDelegate alloc] initWithOwner:weak_factory_.GetWeakPtr()];
  key_session_ = [AVContentKeySession
      contentKeySessionWithKeySystem:AVContentKeySystemFairPlayStreaming];
  [key_session_ setDelegate:key_delegate_ queue:dispatch_get_main_queue()];
  if (!mse_) {
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:net::NSURLWithGURL(url)
                                            options:asset_options];
    [key_session_ addContentKeyRecipient:asset];
    item_ = [AVPlayerItem playerItemWithAsset:asset];
  }
  // MSE: no item until the first session (LoadMseItem()).
  player_ = [AVPlayer playerWithPlayerItem:item_];
  player_.automaticallyWaitsToMinimizeStalling = YES;
  // The page decides when to play.
  player_.actionAtItemEnd = AVPlayerActionAtItemEndPause;

  layer_ = [AVPlayerLayer playerLayerWithPlayer:player_];
  layer_.videoGravity = AVLayerVideoGravityResize;
  layer_.anchorPoint = CGPointZero;
  layer_.frame = CGRectMake(0, 0, kExternalLayerWidth, kExternalLayerHeight);
  // Keep FairPlay content out of screenshots and screen sharing, as Safari
  // does. (AVPlayerLayer already does this for protected content.)

  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  context_ = [CAContext contextWithCGSConnection:CGSMainConnectionID()
                                         options:@{}];
  context_.layer = layer_;
  [CATransaction commit];

  if (item_) {
    observer_ = [[CompassAVFObserver alloc]
        initWithPlayer:player_
                  item:item_
                 owner:weak_factory_.GetWeakPtr()];
  }
}

AVFPlayerImpl::~AVFPlayerImpl() {
  mse_source_.reset();
  [key_session_ setDelegate:nil queue:nil];
  [observer_ stop];
  [player_ pause];
  [player_ replaceCurrentItemWithPlayerItem:nil];
  context_.layer = nil;
}

uint32_t AVFPlayerImpl::ca_context_id() const {
  return context_.contextId;
}

void AVFPlayerImpl::Disconnect() {
  if (on_disconnect_) {
    std::move(on_disconnect_).Run();  // Deletes `this`.
  }
}

void AVFPlayerImpl::SetMseSource(std::unique_ptr<AVFMseSourceImpl> source) {
  mse_source_ = std::move(source);
}

void AVFPlayerImpl::LoadMseItem(const GURL& url,
                                base::TimeDelta time_offset,
                                MseItemCallback ready) {
  DCHECK(mse_);
  [observer_ stop];
  observer_ = nil;
  if (AVURLAsset* old_asset = base::apple::ObjCCast<AVURLAsset>(item_.asset)) {
    [mse_key_session_ removeContentKeyRecipient:old_asset];
  }
  AVURLAsset* asset = [AVURLAsset URLAssetWithURL:net::NSURLWithGURL(url)
                                          options:nil];
  // The CDM's key session when there's a CDM: it has the keys the page
  // already licensed (and they outlive this player). Keys a session already
  // has stay usable for new assets: AVPlayer asks only for those it lacks.
  mse_key_session_ = cdm_ ? cdm_->key_session() : key_session_;
  [mse_key_session_ addContentKeyRecipient:asset];
  item_ = [AVPlayerItem playerItemWithAsset:asset];
  time_offset_ = time_offset;
  ended_ = false;
  mse_ready_callback_ = std::move(ready);
  [player_ replaceCurrentItemWithPlayerItem:item_];
  observer_ = [[CompassAVFObserver alloc] initWithPlayer:player_
                                                   item:item_
                                                  owner:weak_factory_.GetWeakPtr()];
}

base::TimeDelta AVFPlayerImpl::CurrentMediaTime() const {
  return CurrentTime();
}

void AVFPlayerImpl::ReportError(const std::string& message) {
  LOG(ERROR) << "FairPlay MSE: " << message;
  client_->OnError(message);
}

void AVFPlayerImpl::Load(LoadCallback callback) {
  load_callback_ = std::move(callback);
  OnItemStatusChanged();
  if (!load_callback_) {
    return;
  }
  // A FairPlay item only becomes ready to play once it has its key, which
  // needs the page's CDM, which the pipeline only attaches once loading is
  // done. So also finish loading once the playlists give the duration; the
  // video's size comes later (OnNaturalSizeChange).
  auto weak = weak_factory_.GetWeakPtr();
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  AVAsset* asset = item_.asset;
  [asset loadValuesAsynchronouslyForKeys:@[ @"duration" ]
                       completionHandler:^{
                         task_runner->PostTask(
                             FROM_HERE,
                             base::BindOnce(&AVFPlayerImpl::OnAssetDurationLoaded,
                                            weak));
                       }];
}

void AVFPlayerImpl::OnAssetDurationLoaded() {
  if (!load_callback_ || item_.status == AVPlayerItemStatusFailed) {
    return;
  }
  NSError* error = nil;
  if ([item_.asset statusOfValueForKey:@"duration" error:&error] !=
      AVKeyValueStatusLoaded) {
    return;  // The item's status reports the failure.
  }
  ReplyToLoad(ToTimeDelta(item_.asset.duration));
  if (VLOG_IS_ON(1)) {
    LogVariants();
  }
}

void AVFPlayerImpl::LogVariants() {
  if (@available(macOS 12, *)) {
    AVURLAsset* asset = base::apple::ObjCCast<AVURLAsset>(item_.asset);
    if (!asset) {
      return;
    }
    [asset loadValuesAsynchronouslyForKeys:@[ @"variants" ]
                         completionHandler:^{
                           for (AVAssetVariant* variant in asset.variants) {
                             CGSize size =
                                 variant.videoAttributes.presentationSize;
                             VLOG(1) << "AVF variant: " << size.width << "x"
                                     << size.height << " peak "
                                     << variant.peakBitRate << " avg "
                                     << variant.averageBitRate;
                           }
                         }];
  }
}

void AVFPlayerImpl::OnAccessLogChanged() {
  AVPlayerItemAccessLogEvent* event = item_.accessLog.events.lastObject;
  if (!event) {
    return;
  }
  CGSize size = item_.presentationSize;
  VLOG(1) << "AVF access log: indicated " << event.indicatedBitrate
          << " observed " << event.observedBitrate << " switch "
          << event.switchBitrate << " stalls " << event.numberOfStalls
          << " presentation " << size.width << "x" << size.height
          << " obscured(protection) "
          << player_.outputObscuredDueToInsufficientExternalProtection
          << " preferredMaxRes " << item_.preferredMaximumResolution.width
          << "x" << item_.preferredMaximumResolution.height
          << " preferredPeak " << item_.preferredPeakBitRate;
}

void AVFPlayerImpl::OnErrorLogChanged() {
  AVPlayerItemErrorLogEvent* event = item_.errorLog.events.lastObject;
  if (!event) {
    return;
  }
  VLOG(1) << "AVF error log: " << event.errorStatusCode << " "
          << base::SysNSStringToUTF8(event.errorDomain ?: @"") << " "
          << base::SysNSStringToUTF8(event.errorComment ?: @"");
}

void AVFPlayerImpl::SetRate(double rate) {
  requested_rate_ = rate;
  if (rate > 0) {
    ended_ = false;
    player_.rate = rate;
  } else {
    [player_ pause];
  }
  SendTimeUpdate();
}

void AVFPlayerImpl::Seek(base::TimeDelta time, SeekCallback callback) {
  ended_ = false;
  CMTime target =
      CMTimeMakeWithSeconds((time - time_offset_).InSecondsF(), 90000);
  __block SeekCallback block_callback = std::move(callback);
  auto weak = weak_factory_.GetWeakPtr();
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  [player_ seekToTime:target
        toleranceBefore:kCMTimeZero
         toleranceAfter:kCMTimeZero
      completionHandler:^(BOOL) {
        task_runner->PostTask(
            FROM_HERE, base::BindOnce(
                           [](base::WeakPtr<AVFPlayerImpl> player,
                              SeekCallback callback) {
                             if (player) {
                               player->SendTimeUpdate();
                             }
                             std::move(callback).Run();
                           },
                           weak, std::move(block_callback)));
      }];
}

void AVFPlayerImpl::SetVolume(float volume) {
  player_.volume = volume;
}

void AVFPlayerImpl::SetCdm(const base::UnguessableToken& cdm_id,
                           SetCdmCallback callback) {
  AVFCdm* cdm = find_cdm_.Run(cdm_id);
  if (!cdm) {
    std::move(callback).Run(false);
    return;
  }
  cdm_ = cdm->GetWeakPtr();
  // Requests that came before the page set its MediaKeys.
  for (AVContentKeyRequest* request in key_requests_) {
    if (request.status == AVContentKeyRequestStatusRequestingResponse) {
      cdm->OnKeyRequest(request, /*renewal=*/false);
    }
  }
  std::move(callback).Run(true);
}

void AVFPlayerImpl::OnKeyRequest(AVContentKeyRequest* request, bool renewal) {
  [key_requests_ addObject:request];
  std::string identifier;
  if ([request.identifier isKindOfClass:NSString.class]) {
    identifier = base::SysNSStringToUTF8(request.identifier);
  }
  if (!identifier.starts_with("skd://")) {
    LOG(ERROR) << "FairPlay: not an skd:// key request";
    [request processContentKeyResponseError:
                 [NSError errorWithDomain:@"CompassFairPlay" code:1 userInfo:nil]];
    return;
  }
  // The page's "encrypted" event (skd), then its CDM, if it has one already.
  if (!mse_ && !renewal && announced_keys_.insert(identifier).second) {
    client_->OnEncrypted(
        std::vector<uint8_t>(identifier.begin(), identifier.end()));
  }
  if (cdm_) {
    cdm_->OnKeyRequest(request, renewal);
  }
}

void AVFPlayerImpl::OnKeyRequestSucceeded(AVContentKeyRequest* request) {
  if (cdm_) {
    cdm_->OnKeyRequestSucceeded(request);
  }
}

void AVFPlayerImpl::OnKeyRequestFailed(AVContentKeyRequest* request,
                                       NSError* error) {
  if (cdm_) {
    cdm_->OnKeyRequestFailed(request, error);
  }
}

void AVFPlayerImpl::OnItemStatusChanged() {
  if (!item_) {
    return;
  }
  switch (item_.status) {
    case AVPlayerItemStatusUnknown:
      return;
    case AVPlayerItemStatusFailed: {
      std::string message = base::SysNSStringToUTF8(
          item_.error.localizedDescription ?: @"AVPlayerItem failed");
      LOG(ERROR) << "AVFoundation HLS: " << message;
      if (mse_ready_callback_) {
        std::move(mse_ready_callback_).Run(false, message);
      } else if (load_callback_) {
        std::move(load_callback_).Run(false, nullptr, message);
      } else {
        client_->OnError(message);
      }
      return;
    }
    case AVPlayerItemStatusReadyToPlay:
      break;
  }
  if (mse_) {
    if (mse_ready_callback_) {
      std::move(mse_ready_callback_).Run(true, std::string());
    }
    OnPresentationSizeChanged();
    OnPlaybackStateChanged();
    return;
  }
  if (!load_callback_) {
    // Loading finished before the item was ready (FairPlay, see Load()).
    OnPresentationSizeChanged();
    OnDurationChanged();
    OnPlaybackStateChanged();
    return;
  }
  ReplyToLoad(Duration());
}

void AVFPlayerImpl::ReplyToLoad(base::TimeDelta duration) {
  auto metadata = mojom::AVFMetadata::New();
  metadata->duration = duration;
  for (AVPlayerItemTrack* track in item_.tracks) {
    NSString* type = track.assetTrack.mediaType;
    if ([type isEqualToString:AVMediaTypeVideo]) {
      metadata->has_video = true;
    } else if ([type isEqualToString:AVMediaTypeAudio]) {
      metadata->has_audio = true;
    }
  }
  CGSize size = item_.presentationSize;
  natural_size_ = gfx::Size(std::lround(size.width), std::lround(size.height));
  // HLS often lists no tracks until playback starts; a size means video.
  if (!natural_size_.IsEmpty()) {
    metadata->has_video = true;
  }
  if (!metadata->has_video && !metadata->has_audio) {
    // Assume both rather than fail; the pipeline needs at least one stream.
    metadata->has_video = true;
    metadata->has_audio = true;
  }
  metadata->natural_size = natural_size_;
  std::move(load_callback_).Run(true, std::move(metadata), std::string());
  OnPlaybackStateChanged();
  OnLoadedRangesChanged();
}

void AVFPlayerImpl::OnPresentationSizeChanged() {
  CGSize size = item_.presentationSize;
  gfx::Size natural_size(std::lround(size.width), std::lround(size.height));
  if (natural_size.IsEmpty() || natural_size == natural_size_) {
    return;
  }
  natural_size_ = natural_size;
  VLOG(1) << "AVF presentation size " << natural_size_.ToString()
          << " obscured(protection) "
          << player_.outputObscuredDueToInsufficientExternalProtection;
  client_->OnNaturalSizeChange(natural_size_);
}

void AVFPlayerImpl::OnLoadedRangesChanged() {
  if (mse_) {
    return;
  }
  std::vector<mojom::AVFTimeRangePtr> ranges;
  for (NSValue* value in item_.loadedTimeRanges) {
    CMTimeRange range = value.CMTimeRangeValue;
    ranges.push_back(mojom::AVFTimeRange::New(
        ToTimeDelta(range.start), ToTimeDelta(CMTimeRangeGetEnd(range))));
  }
  client_->OnBufferedRangesChange(std::move(ranges));
}

void AVFPlayerImpl::OnDurationChanged() {
  if (mse_) {
    return;
  }
  client_->OnDurationChange(Duration());
}

void AVFPlayerImpl::OnPlaybackStateChanged() {
  // Enough data: AVPlayer is playing, or is paused and ready (it buffers by
  // itself once asked to play, and reports the wait as a stall then).
  bool have_enough = item_ && item_.status == AVPlayerItemStatusReadyToPlay &&
                     (requested_rate_ == 0 || ended_ ||
                      player_.timeControlStatus ==
                          AVPlayerTimeControlStatusPlaying);
  if (have_enough_ != have_enough) {
    have_enough_ = have_enough;
    client_->OnBufferingStateChange(have_enough);
  }
  SendTimeUpdate();
}

void AVFPlayerImpl::OnPlayedToEnd() {
  ended_ = true;
  SendTimeUpdate();
  client_->OnEnded();
}

void AVFPlayerImpl::OnFailedToPlayToEnd(const std::string& message) {
  LOG(ERROR) << "AVFoundation HLS: " << message;
  client_->OnError(message);
}

void AVFPlayerImpl::OnPeriodicTime() {
  SendTimeUpdate();
}

void AVFPlayerImpl::SendTimeUpdate() {
  base::TimeDelta time = CurrentTime();
  double rate = EffectiveRate();
  // The renderer extrapolates between updates; only resend when that would
  // drift: on rate changes, and every periodic tick while playing.
  if (rate == last_sent_rate_ && rate == 0 && time == last_sent_time_) {
    return;
  }
  last_sent_time_ = time;
  last_sent_rate_ = rate;
  client_->OnTimeUpdate(time, rate);
}

base::TimeDelta AVFPlayerImpl::CurrentTime() const {
  if (!item_) {
    return time_offset_;
  }
  base::TimeDelta time = ToTimeDelta(item_.currentTime);
  return (time == media::kInfiniteDuration ? base::TimeDelta() : time) +
         time_offset_;
}

double AVFPlayerImpl::EffectiveRate() const {
  if (ended_ ||
      player_.timeControlStatus != AVPlayerTimeControlStatusPlaying) {
    return 0;
  }
  return player_.rate;
}

base::TimeDelta AVFPlayerImpl::Duration() const {
  return ToTimeDelta(item_.duration);
}

}  // namespace compass::avf
