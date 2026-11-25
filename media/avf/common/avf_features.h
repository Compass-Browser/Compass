// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_COMMON_AVF_FEATURES_H_
#define COMPASS_MEDIA_AVF_COMMON_AVF_FEATURES_H_

#include <string_view>

#include "base/component_export.h"
#include "base/feature_list.h"

class GURL;

namespace compass::avf {

// Plays native HLS (<video src="*.m3u8">) with AVFoundation's AVPlayer
// instead of Chromium's HLS demuxer. macOS only.
COMPONENT_EXPORT(COMPASS_AVF_COMMON) BASE_DECLARE_FEATURE(kAVFoundationHls);

// Offers FairPlay Streaming (com.apple.fps) through EME, for native HLS
// played by AVPlayer. Needs kAVFoundationHls. macOS only.
COMPONENT_EXPORT(COMPASS_AVF_COMMON) BASE_DECLARE_FEATURE(kFairPlay);

// FairPlay for MSE (docs/design/fairplay.md): com.apple.fps for MP4 through
// Media Source Extensions too, played by AVPlayer from a local HLS copy of
// the stream. Needs kFairPlay. macOS only.
COMPONENT_EXPORT(COMPASS_AVF_COMMON) BASE_DECLARE_FEATURE(kFairPlayMse);

// Whether a media element loading `url` should play it with AVPlayer.
COMPONENT_EXPORT(COMPASS_AVF_COMMON) bool ShouldUseAVFoundation(const GURL& url);

// Whether pages get com.apple.fps.
COMPONENT_EXPORT(COMPASS_AVF_COMMON) bool IsFairPlayEnabled();
COMPONENT_EXPORT(COMPASS_AVF_COMMON) bool IsFairPlayMseEnabled();

// Whether `user_agent` is Safari's, as pages presenting as Safari send
// (//compass/browser/present_as_safari). Such pages also get FairPlay's
// "cenc" probes and Safari's legacy WebKitMediaKeys.
COMPONENT_EXPORT(COMPASS_AVF_COMMON)
bool IsSafariUserAgent(std::string_view user_agent);

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_COMMON_AVF_FEATURES_H_
