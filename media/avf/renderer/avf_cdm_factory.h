// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_RENDERER_AVF_CDM_FACTORY_H_
#define COMPASS_MEDIA_AVF_RENDERER_AVF_CDM_FACTORY_H_

#include <memory>

#include "base/functional/callback.h"
#include "media/base/cdm_factory.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"

namespace compass::avf {

// The renderer's CDM factory for com.apple.fps: a media::MojoCdm talking to
// the FairPlay CDM in the AVFoundation utility process (made through the
// frame's AVFPlayerFactory). Other key systems go to the factory it wraps.
class AVFCdmFactory : public media::CdmFactory {
 public:
  using BindInterfaceCallback =
      base::RepeatingCallback<void(mojo::GenericPendingReceiver)>;

  AVFCdmFactory(std::unique_ptr<media::CdmFactory> other,
                BindInterfaceCallback bind_interface);
  AVFCdmFactory(const AVFCdmFactory&) = delete;
  AVFCdmFactory& operator=(const AVFCdmFactory&) = delete;
  ~AVFCdmFactory() override;

  // media::CdmFactory:
  void Create(const media::CdmConfig& cdm_config,
              const media::SessionMessageCB& session_message_cb,
              const media::SessionClosedCB& session_closed_cb,
              const media::SessionKeysChangeCB& session_keys_change_cb,
              const media::SessionExpirationUpdateCB&
                  session_expiration_update_cb,
              media::CdmCreatedCB cdm_created_cb) override;

 private:
  const std::unique_ptr<media::CdmFactory> other_;
  const BindInterfaceCallback bind_interface_;
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_RENDERER_AVF_CDM_FACTORY_H_
