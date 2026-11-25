// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/fairplay_key_system_info.h"

#include "compass/media/avf/common/avf_features.h"
#include "media/base/eme_constants.h"

namespace compass::avf {

FairPlayKeySystemInfo::FairPlayKeySystemInfo(bool safari) : safari_(safari) {}
FairPlayKeySystemInfo::~FairPlayKeySystemInfo() = default;

std::string FairPlayKeySystemInfo::GetBaseKeySystemName() const {
  return kFairPlayKeySystem;
}

bool FairPlayKeySystemInfo::IsSupportedKeySystem(
    const std::string& key_system) const {
  // As in Safari (CDMFactoryFairPlayStreaming::supportsKeySystem): players
  // ask for "com.apple.fps.1_0" ... "com.apple.fps.3_0" too (Bitmovin does).
  return key_system == kFairPlayKeySystem ||
         key_system.starts_with(std::string(kFairPlayKeySystem) + ".");
}

bool FairPlayKeySystemInfo::ShouldUseBaseKeySystemName() const {
  // The CDM is made for "com.apple.fps" whatever the page asked for.
  return true;
}

bool FairPlayKeySystemInfo::IsSupportedInitDataType(
    media::EmeInitDataType init_data_type) const {
  // "cenc": MSE (FairPlay for MSE), and what players probe on pages
  // presenting as Safari.
  return init_data_type == media::EmeInitDataType::SKD ||
         (init_data_type == media::EmeInitDataType::CENC &&
          (safari_ || IsFairPlayMseEnabled()));
}

media::EmeConfig::Rule FairPlayKeySystemInfo::GetEncryptionSchemeConfigRule(
    media::EncryptionScheme encryption_scheme) const {
  // FairPlay Streaming is 'cbcs' (SAMPLE-AES in HLS), but pages that name no
  // scheme are checked against 'cenc' (Chromium's default), and AVFoundation,
  // not the page's configuration, decides what the stream is. MSE content
  // in 'cenc' fails when it's repackaged (mse::HlsSession).
  return encryption_scheme == media::EncryptionScheme::kUnencrypted
             ? media::EmeConfig::UnsupportedRule()
             : media::EmeConfig::SupportedRule();
}

media::SupportedCodecs FairPlayKeySystemInfo::GetSupportedCodecs() const {
  // MP4 is MSE, which AVPlayer plays through FairPlay for MSE: what it can
  // repackage (mse::HlsSession). Without it, an MSE player that picked
  // com.apple.fps would hand its samples to Chromium's decryptor, which can't
  // use this CDM (Apple TV+ did, and failed), so only the playlist's types
  // are offered (key_system_config_selector.cc's patch) and MSE players go
  // on to Widevine; pages presenting as Safari still get MP4, for players
  // that probe it so there (Bitmovin).
  if (IsFairPlayMseEnabled()) {
    return media::EME_CODEC_AVC1 | media::EME_CODEC_HEVC_PROFILE_MAIN |
           media::EME_CODEC_HEVC_PROFILE_MAIN10 | media::EME_CODEC_AAC;
  }
  return safari_ ? media::EME_CODEC_MP4_ALL : media::EME_CODEC_NONE;
}

media::EmeConfig::Rule FairPlayKeySystemInfo::GetRobustnessConfigRule(
    const std::string& key_system,
    media::EmeMediaType media_type,
    const std::string& requested_robustness,
    const bool* hw_secure_requirement) const {
  return requested_robustness.empty() ? media::EmeConfig::SupportedRule()
                                      : media::EmeConfig::UnsupportedRule();
}

media::EmeConfig::Rule
FairPlayKeySystemInfo::GetPersistentLicenseSessionSupport() const {
  return media::EmeConfig::UnsupportedRule();
}

media::EmeFeatureSupport FairPlayKeySystemInfo::GetPersistentStateSupport()
    const {
  return media::EmeFeatureSupport::NOT_SUPPORTED;
}

media::EmeFeatureSupport
FairPlayKeySystemInfo::GetDistinctiveIdentifierSupport() const {
  return media::EmeFeatureSupport::NOT_SUPPORTED;
}

}  // namespace compass::avf
