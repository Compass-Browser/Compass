// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_COMMON_AVF_FEATURES_H_
#define COMPASS_MEDIA_AVF_COMMON_AVF_FEATURES_H_

#include "base/feature_list.h"

class GURL;

namespace compass::avf {

// Plays native HLS (<video src="*.m3u8">) with AVFoundation's AVPlayer
// instead of Chromium's HLS demuxer. macOS only.
BASE_DECLARE_FEATURE(kAVFoundationHls);

// Whether a media element loading `url` should play it with AVPlayer.
bool ShouldUseAVFoundation(const GURL& url);

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_COMMON_AVF_FEATURES_H_
