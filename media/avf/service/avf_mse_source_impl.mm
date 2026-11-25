// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/avf_mse_source_impl.h"

#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "compass/media/avf/service/avf_player_impl.h"
#include "compass/media/avf/service/mse/loopback_server.h"
#include "media/base/decrypt_config.h"
#include "media/base/encryption_pattern.h"

namespace compass::avf {
namespace {

// AVPlayer starts once the session reaches this far past the start.
constexpr base::TimeDelta kStartAhead = base::Seconds(1);
// Segments AVPlayer played this long ago are freed.
constexpr base::TimeDelta kKeepBehind = base::Seconds(10);

mse::MseSample ToSample(mojom::AVFSamplePtr in) {
  mse::MseSample out;
  base::span<const uint8_t> data(in->data);
  out.data.assign(data.begin(), data.end());
  out.timestamp = in->timestamp;
  out.duration = in->duration;
  out.is_key_frame = in->is_key_frame;
  if (in->encryption) {
    const mojom::AVFSampleEncryption& encryption = *in->encryption;
    std::string key_id(encryption.key_id.begin(), encryption.key_id.end());
    std::string iv(encryption.iv.begin(), encryption.iv.end());
    std::vector<media::SubsampleEntry> subsamples;
    for (const auto& subsample : encryption.subsamples) {
      subsamples.emplace_back(subsample->clear_bytes, subsample->cypher_bytes);
    }
    if (encryption.cbcs) {
      std::optional<media::EncryptionPattern> pattern;
      if (encryption.has_pattern) {
        pattern = media::EncryptionPattern::Create(encryption.crypt_byte_block,
                                                   encryption.skip_byte_block);
      }
      out.decrypt_config = media::DecryptConfig::CreateCbcsConfig(
          key_id, iv, std::move(subsamples), pattern);
    } else {
      out.decrypt_config = media::DecryptConfig::CreateCencConfig(
          key_id, iv, std::move(subsamples));
    }
  }
  return out;
}

mse::HlsSession::Track ToTrack(mojom::AVFTrack track) {
  return track == mojom::AVFTrack::kVideo ? mse::HlsSession::Track::kVideo
                                          : mse::HlsSession::Track::kAudio;
}

}  // namespace

AVFMseSourceImpl::AVFMseSourceImpl(
    mojo::PendingReceiver<mojom::AVFMseSource> receiver,
    AVFPlayerImpl* player)
    : receiver_(this, std::move(receiver)), player_(player) {}

AVFMseSourceImpl::~AVFMseSourceImpl() {
  EndSession();
}

void AVFMseSourceImpl::EndSession() {
  if (url_.is_valid()) {
    if (mse::LoopbackServer* server = mse::LoopbackServer::Get()) {
      server->Unregister(url_);
    }
    url_ = GURL();
  }
  if (start_callback_) {
    std::move(start_callback_).Run(false, "Superseded by a new session.");
  }
}

void AVFMseSourceImpl::StartSession(
    base::TimeDelta start,
    const std::optional<media::AudioDecoderConfig>& audio,
    const std::optional<media::VideoDecoderConfig>& video,
    StartSessionCallback callback) {
  EndSession();
  ++session_id_;
  session_ = base::MakeRefCounted<mse::HlsSession>(audio, video);
  start_ = start;
  item_loading_ = false;
  error_reported_ = false;
  start_callback_ = std::move(callback);
  Update();
}

void AVFMseSourceImpl::SetAudioConfig(const media::AudioDecoderConfig& config) {
  if (session_) {
    session_->SetAudioConfig(config);
    Update();
  }
}

void AVFMseSourceImpl::SetVideoConfig(const media::VideoDecoderConfig& config) {
  if (session_) {
    session_->SetVideoConfig(config);
    Update();
  }
}

void AVFMseSourceImpl::Append(mojom::AVFTrack track,
                              std::vector<mojom::AVFSamplePtr> samples) {
  if (!session_) {
    return;
  }
  std::vector<mse::MseSample> converted;
  converted.reserve(samples.size());
  for (auto& sample : samples) {
    converted.push_back(ToSample(std::move(sample)));
  }
  session_->Append(ToTrack(track), std::move(converted));
  Update();
}

void AVFMseSourceImpl::EndOfStream(mojom::AVFTrack track) {
  if (session_) {
    session_->EndOfStream(ToTrack(track));
    Update();
  }
}

void AVFMseSourceImpl::Update() {
  if (!session_) {
    return;
  }
  if (!session_->error().empty()) {
    if (start_callback_) {
      std::move(start_callback_).Run(false, session_->error());
    } else if (!error_reported_) {
      player_->ReportError(session_->error());
    }
    error_reported_ = true;
    return;
  }
  VLOG(2) << "FairPlay MSE: session has " << session_->segment_count()
          << " segments to " << session_->segmented_end().InSecondsF()
          << "s, start " << start_.InSecondsF() << "s";
  if (!item_loading_ && session_->origin() &&
      (session_->ended() ||
       session_->segmented_end() >= start_ + kStartAhead)) {
    mse::LoopbackServer* server = mse::LoopbackServer::Get();
    if (!server) {
      if (start_callback_) {
        std::move(start_callback_)
            .Run(false, "Can't serve the stream to AVPlayer.");
      }
      return;
    }
    item_loading_ = true;
    url_ = server->Register(session_);
    VLOG(1) << "FairPlay MSE: AVPlayer loads " << url_.spec();
    player_->LoadMseItem(
        url_, *session_->origin(),
        base::BindOnce(&AVFMseSourceImpl::OnItemReady,
                       weak_factory_.GetWeakPtr(), session_id_));
  }
  if (item_loading_) {
    session_->DropBefore(player_->CurrentMediaTime() - kKeepBehind);
  }
}

void AVFMseSourceImpl::OnItemReady(int session,
                                   bool success,
                                   const std::string& error) {
  if (session != session_id_ || !start_callback_) {
    return;
  }
  if (!success) {
    std::move(start_callback_).Run(false, error);
    return;
  }
  player_->Seek(start_, base::BindOnce(&AVFMseSourceImpl::OnSeeked,
                                       weak_factory_.GetWeakPtr(), session));
}

void AVFMseSourceImpl::OnSeeked(int session) {
  if (session != session_id_ || !start_callback_) {
    return;
  }
  std::move(start_callback_).Run(true, std::string());
}

}  // namespace compass::avf
