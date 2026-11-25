// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_RENDERER_FAIRPLAY_KEY_SYSTEM_INFO_H_
#define COMPASS_MEDIA_AVF_RENDERER_FAIRPLAY_KEY_SYSTEM_INFO_H_

#include <string>

#include "media/base/key_system_info.h"

namespace compass::avf {

inline constexpr char kFairPlayKeySystem[] = "com.apple.fps";

// com.apple.fps, as Safari offers it with native HLS: temporary sessions, no
// robustness levels, and "skd" init data only. Pages that would use FairPlay
// through MSE (hls.js, Shaka, video.js ask for "cenc"/"sinf") get no
// matching configuration and keep using Widevine, which they can play.
// Pages presenting as Safari get "cenc" too, as in Safari, because players
// that only use FairPlay there (Bitmovin) probe it with "cenc".
class FairPlayKeySystemInfo : public media::KeySystemInfo {
 public:
  // `safari`: the page presents itself as Safari, so accept the init data
  // types Safari's FairPlay does ("skd" and "cenc"); otherwise "skd" only.
  explicit FairPlayKeySystemInfo(bool safari);
  ~FairPlayKeySystemInfo() override;

  std::string GetBaseKeySystemName() const override;
  bool IsSupportedKeySystem(const std::string& key_system) const override;
  bool ShouldUseBaseKeySystemName() const override;
  bool IsSupportedInitDataType(
      media::EmeInitDataType init_data_type) const override;
  media::EmeConfig::Rule GetEncryptionSchemeConfigRule(
      media::EncryptionScheme encryption_scheme) const override;
  media::SupportedCodecs GetSupportedCodecs() const override;
  media::EmeConfig::Rule GetRobustnessConfigRule(
      const std::string& key_system,
      media::EmeMediaType media_type,
      const std::string& requested_robustness,
      const bool* hw_secure_requirement) const override;
  media::EmeConfig::Rule GetPersistentLicenseSessionSupport() const override;
  media::EmeFeatureSupport GetPersistentStateSupport() const override;
  media::EmeFeatureSupport GetDistinctiveIdentifierSupport() const override;

 private:
  const bool safari_;
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_RENDERER_FAIRPLAY_KEY_SYSTEM_INFO_H_
