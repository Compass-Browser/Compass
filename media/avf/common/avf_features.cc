// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/common/avf_features.h"

#include "build/build_config.h"
#include "url/gurl.h"

namespace compass::avf {

BASE_FEATURE(kAVFoundationHls, "CompassAVFoundationHls",
#if BUILDFLAG(IS_MAC)
             base::FEATURE_ENABLED_BY_DEFAULT
#else
             base::FEATURE_DISABLED_BY_DEFAULT
#endif
);

BASE_FEATURE(kFairPlay, "CompassFairPlay",
#if BUILDFLAG(IS_MAC)
             base::FEATURE_ENABLED_BY_DEFAULT
#else
             base::FEATURE_DISABLED_BY_DEFAULT
#endif
);

bool IsFairPlayEnabled() {
#if BUILDFLAG(IS_MAC)
  return base::FeatureList::IsEnabled(kAVFoundationHls) &&
         base::FeatureList::IsEnabled(kFairPlay);
#else
  return false;
#endif
}

bool ShouldUseAVFoundation(const GURL& url) {
#if BUILDFLAG(IS_MAC)
  if (!base::FeatureList::IsEnabled(kAVFoundationHls)) {
    return false;
  }
  // The same test as media::DemuxerManager::IsManifestDemuxerURL(), which
  // picks Chromium's HLS demuxer.
  return url.SchemeIsHTTPOrHTTPS() && url.path().ends_with(".m3u8");
#else
  return false;
#endif
}

}  // namespace compass::avf
