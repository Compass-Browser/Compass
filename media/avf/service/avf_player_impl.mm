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
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "compass/ui/external_layer/external_layer_constants.h"
#include "media/base/timestamp_constants.h"
#include "net/base/apple/url_conversions.h"
#include "ui/base/cocoa/remote_layer_api.h"

// Bridges KVO and notifications from AVFoundation to AVFPlayerImpl, on the
// main thread (where AVFPlayerImpl lives).
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
    base::OnceClosure on_disconnect)
    : receiver_(this, std::move(receiver)),
      client_(std::move(client)),
      on_disconnect_(std::move(on_disconnect)) {
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

  AVURLAsset* asset = [AVURLAsset URLAssetWithURL:net::NSURLWithGURL(url)
                                          options:asset_options];
  item_ = [AVPlayerItem playerItemWithAsset:asset];
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

  observer_ = [[CompassAVFObserver alloc]
      initWithPlayer:player_
                item:item_
               owner:weak_factory_.GetWeakPtr()];
}

AVFPlayerImpl::~AVFPlayerImpl() {
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

void AVFPlayerImpl::Load(LoadCallback callback) {
  load_callback_ = std::move(callback);
  OnItemStatusChanged();
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
  CMTime target = CMTimeMakeWithSeconds(time.InSecondsF(), 90000);
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

void AVFPlayerImpl::OnItemStatusChanged() {
  switch (item_.status) {
    case AVPlayerItemStatusUnknown:
      return;
    case AVPlayerItemStatusFailed: {
      std::string message = base::SysNSStringToUTF8(
          item_.error.localizedDescription ?: @"AVPlayerItem failed");
      LOG(ERROR) << "AVFoundation HLS: " << message;
      if (load_callback_) {
        std::move(load_callback_).Run(false, nullptr, message);
      } else {
        client_->OnError(message);
      }
      return;
    }
    case AVPlayerItemStatusReadyToPlay:
      break;
  }
  if (!load_callback_) {
    return;
  }
  auto metadata = mojom::AVFMetadata::New();
  metadata->duration = Duration();
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
  client_->OnNaturalSizeChange(natural_size_);
}

void AVFPlayerImpl::OnLoadedRangesChanged() {
  std::vector<mojom::AVFTimeRangePtr> ranges;
  for (NSValue* value in item_.loadedTimeRanges) {
    CMTimeRange range = value.CMTimeRangeValue;
    ranges.push_back(mojom::AVFTimeRange::New(
        ToTimeDelta(range.start), ToTimeDelta(CMTimeRangeGetEnd(range))));
  }
  client_->OnBufferedRangesChange(std::move(ranges));
}

void AVFPlayerImpl::OnDurationChanged() {
  client_->OnDurationChange(Duration());
}

void AVFPlayerImpl::OnPlaybackStateChanged() {
  // Enough data: AVPlayer is playing, or is paused and ready (it buffers by
  // itself once asked to play, and reports the wait as a stall then).
  bool have_enough = item_.status == AVPlayerItemStatusReadyToPlay &&
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
  base::TimeDelta time = ToTimeDelta(item_.currentTime);
  return time == media::kInfiniteDuration ? base::TimeDelta() : time;
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
