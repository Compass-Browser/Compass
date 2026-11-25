// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_AVF_CDM_H_
#define COMPASS_MEDIA_AVF_SERVICE_AVF_CDM_H_

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "media/mojo/mojom/content_decryption_module.mojom.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"

#if defined(__OBJC__)
@class AVContentKeyRequest;
@class NSMutableDictionary;
@class NSError;
#endif

namespace compass::avf {

// A FairPlay Streaming CDM (com.apple.fps) in the AVFoundation utility
// process, for one MediaKeys. AVPlayers that use it (AVFPlayerImpl::SetCdm)
// hand it their AVContentKeySession's key requests; the page's sessions turn
// them into SPCs (license requests) and take back CKCs, as in Safari:
//
//   AVPlayer needs skd://k  ->  page gets "encrypted" (skd, "skd://k")
//   generateRequest("skd", "skd://k")  ->  SPC in a "message" event
//   update(CKC)  ->  the key is usable, AVPlayer plays.
//
// Only temporary sessions; the key id is the skd:// URI, as in WebKit.
class AVFCdm : public media::mojom::ContentDecryptionModule {
 public:
  AVFCdm(const base::UnguessableToken& id,
         mojo::PendingReceiver<media::mojom::ContentDecryptionModule> receiver,
         base::OnceClosure on_disconnect);
  AVFCdm(const AVFCdm&) = delete;
  AVFCdm& operator=(const AVFCdm&) = delete;
  ~AVFCdm() override;

  const base::UnguessableToken& id() const { return id_; }
  base::WeakPtr<AVFCdm> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

#if defined(__OBJC__)
  // From AVFPlayerImpl's AVContentKeySession, on the main thread.
  void OnKeyRequest(AVContentKeyRequest* request, bool renewal);
  void OnKeyRequestSucceeded(AVContentKeyRequest* request);
  void OnKeyRequestFailed(AVContentKeyRequest* request, NSError* error);
#endif

  // media::mojom::ContentDecryptionModule:
  void SetClient(
      mojo::PendingAssociatedRemote<media::mojom::ContentDecryptionModuleClient>
          client) override;
  void SetServerCertificate(const std::vector<uint8_t>& certificate,
                            SetServerCertificateCallback callback) override;
  void GetStatusForPolicy(media::HdcpVersion min_hdcp_version,
                          GetStatusForPolicyCallback callback) override;
  void CreateSessionAndGenerateRequest(
      media::CdmSessionType session_type,
      media::EmeInitDataType init_data_type,
      const std::vector<uint8_t>& init_data,
      CreateSessionAndGenerateRequestCallback callback) override;
  void LoadSession(media::CdmSessionType session_type,
                   const std::string& session_id,
                   LoadSessionCallback callback) override;
  void UpdateSession(const std::string& session_id,
                     const std::vector<uint8_t>& response,
                     UpdateSessionCallback callback) override;
  void CloseSession(const std::string& session_id,
                    CloseSessionCallback callback) override;
  void RemoveSession(const std::string& session_id,
                     RemoveSessionCallback callback) override;

 private:
  struct Session;

  // The session for skd:// URI `identifier`, or null.
  Session* SessionForIdentifier(const std::string& identifier);
  void RequestLicense(const std::string& session_id, bool renewal);
  void OnLicenseRequestData(const std::string& session_id,
                            bool renewal,
                            std::vector<uint8_t> spc,
                            const std::string& error);
  void SendKeyStatus(const std::string& session_id,
                     const std::string& identifier,
                     media::CdmKeyInformation::KeyStatus status);

  const base::UnguessableToken id_;
  mojo::Receiver<media::mojom::ContentDecryptionModule> receiver_;
  mojo::AssociatedRemote<media::mojom::ContentDecryptionModuleClient> client_;
  base::OnceClosure on_disconnect_;

  std::vector<uint8_t> certificate_;
  std::map<std::string, std::unique_ptr<Session>> sessions_;
  int next_session_id_ = 1;

  // Key requests no session asked for yet, by skd:// URI.
  NSMutableDictionary* __strong pending_requests_;

  base::WeakPtrFactory<AVFCdm> weak_factory_{this};
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_SERVICE_AVF_CDM_H_
