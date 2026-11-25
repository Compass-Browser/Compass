// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/renderer/fairplay_key_system_info.h"

#include "media/base/eme_constants.h"

namespace compass::avf {

FairPlayKeySystemInfo::FairPlayKeySystemInfo() = default;
FairPlayKeySystemInfo::~FairPlayKeySystemInfo() = default;

std::string FairPlayKeySystemInfo::GetBaseKeySystemName() const {
  return kFairPlayKeySystem;
}

bool FairPlayKeySystemInfo::IsSupportedInitDataType(
    media::EmeInitDataType init_data_type) const {
  return init_data_type == media::EmeInitDataType::SKD;
}

media::EmeConfig::Rule FairPlayKeySystemInfo::GetEncryptionSchemeConfigRule(
    media::EncryptionScheme encryption_scheme) const {
  // FairPlay Streaming is 'cbcs' (SAMPLE-AES in HLS), but pages that name no
  // scheme are checked against 'cenc' (Chromium's default), and AVFoundation,
  // not the page's configuration, decides what the stream is. MSE players
  // asking for 'cenc' are kept away by the "skd"-only init data type.
  return encryption_scheme == media::EncryptionScheme::kUnencrypted
             ? media::EmeConfig::UnsupportedRule()
             : media::EmeConfig::SupportedRule();
}

media::SupportedCodecs FairPlayKeySystemInfo::GetSupportedCodecs() const {
  // For pages that describe the stream as MP4; AVPlayer decodes it.
  return media::EME_CODEC_MP4_ALL;
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
