// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/common/avf_features.h"

#include "base/test/scoped_feature_list.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::avf {
namespace {

#if BUILDFLAG(IS_MAC)

TEST(AVFFeaturesTest, PlaysHlsUrlsOnly) {
  EXPECT_TRUE(ShouldUseAVFoundation(GURL("https://example.com/a/master.m3u8")));
  EXPECT_TRUE(ShouldUseAVFoundation(GURL("http://example.com/x.m3u8?token=1")));
  EXPECT_FALSE(ShouldUseAVFoundation(GURL("https://example.com/video.mp4")));
  EXPECT_FALSE(ShouldUseAVFoundation(GURL("https://example.com/m3u8")));
  EXPECT_FALSE(ShouldUseAVFoundation(GURL("file:///tmp/a.m3u8")));
  EXPECT_FALSE(ShouldUseAVFoundation(GURL("blob:https://example.com/uuid")));
}

TEST(AVFFeaturesTest, FeatureTurnsItOff) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(kAVFoundationHls);
  EXPECT_FALSE(ShouldUseAVFoundation(GURL("https://example.com/a.m3u8")));
}

#else

TEST(AVFFeaturesTest, OnlyOnMac) {
  EXPECT_FALSE(ShouldUseAVFoundation(GURL("https://example.com/a.m3u8")));
}

#endif

}  // namespace
}  // namespace compass::avf
