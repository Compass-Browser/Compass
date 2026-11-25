// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/avf_cdm.h"

#import <AVFoundation/AVFoundation.h>

#include <utility>

#include "base/apple/foundation_util.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/cdm_key_information.h"
#include "media/base/cdm_promise.h"
#include "media/base/content_decryption_module.h"

namespace compass::avf {
namespace {

using Exception = media::CdmPromise::Exception;

media::mojom::CdmPromiseResultPtr Success() {
  auto result = media::mojom::CdmPromiseResult::New();
  result->success = true;
  result->exception = Exception::NOT_SUPPORTED_ERROR;
  return result;
}

media::mojom::CdmPromiseResultPtr Failure(Exception exception,
                                          const std::string& message) {
  auto result = media::mojom::CdmPromiseResult::New();
  result->success = false;
  result->exception = exception;
  result->error_message = message;
  return result;
}

std::string IdentifierOf(AVContentKeyRequest* request) {
  if ([request.identifier isKindOfClass:NSString.class]) {
    return base::SysNSStringToUTF8(request.identifier);
  }
  if ([request.identifier isKindOfClass:NSData.class]) {
    auto bytes = base::apple::NSDataToSpan(request.identifier);
    return std::string(bytes.begin(), bytes.end());
  }
  return std::string();
}

NSData* ToNSData(base::span<const uint8_t> bytes) {
  return [NSData dataWithBytes:bytes.data() length:bytes.size()];
}

}  // namespace

struct AVFCdm::Session {
  std::string identifier;  // The skd:// URI.
  AVContentKeyRequest* __strong request;
  bool license_requested = false;
};

AVFCdm::AVFCdm(
    const base::UnguessableToken& id,
    mojo::PendingReceiver<media::mojom::ContentDecryptionModule> receiver,
    base::OnceClosure on_disconnect)
    : id_(id),
      receiver_(this, std::move(receiver)),
      on_disconnect_(std::move(on_disconnect)),
      pending_requests_([NSMutableDictionary dictionary]) {
  receiver_.set_disconnect_handler(base::BindOnce(
      [](AVFCdm* cdm) {
        if (cdm->on_disconnect_) {
          std::move(cdm->on_disconnect_).Run();  // Deletes `cdm`.
        }
      },
      base::Unretained(this)));
}

AVFCdm::~AVFCdm() = default;

void AVFCdm::SetClient(
    mojo::PendingAssociatedRemote<media::mojom::ContentDecryptionModuleClient>
        client) {
  client_.Bind(std::move(client));
}

void AVFCdm::SetServerCertificate(const std::vector<uint8_t>& certificate,
                                  SetServerCertificateCallback callback) {
  if (certificate.empty()) {
    std::move(callback).Run(
        Failure(Exception::TYPE_ERROR, "Empty server certificate."));
    return;
  }
  certificate_ = certificate;
  std::move(callback).Run(Success());
}

void AVFCdm::GetStatusForPolicy(media::HdcpVersion min_hdcp_version,
                                GetStatusForPolicyCallback callback) {
  // AVFoundation enforces the CKC's output protection itself.
  std::move(callback).Run(Success(), media::CdmKeyInformation::USABLE);
}

void AVFCdm::CreateSessionAndGenerateRequest(
    media::CdmSessionType session_type,
    media::EmeInitDataType init_data_type,
    const std::vector<uint8_t>& init_data,
    CreateSessionAndGenerateRequestCallback callback) {
  if (session_type != media::CdmSessionType::kTemporary) {
    std::move(callback).Run(
        Failure(Exception::NOT_SUPPORTED_ERROR,
                "Only temporary sessions are supported."),
        std::string());
    return;
  }
  if (init_data_type != media::EmeInitDataType::SKD) {
    std::move(callback).Run(Failure(Exception::NOT_SUPPORTED_ERROR,
                                    "Only 'skd' init data is supported."),
                            std::string());
    return;
  }
  if (certificate_.empty()) {
    // FairPlay needs the application certificate to make the SPC.
    std::move(callback).Run(
        Failure(Exception::INVALID_STATE_ERROR,
                "Call setServerCertificate() before generateRequest()."),
        std::string());
    return;
  }
  std::string identifier(init_data.begin(), init_data.end());
  if (!identifier.starts_with("skd://")) {
    std::move(callback).Run(
        Failure(Exception::TYPE_ERROR, "'skd' init data must be an skd:// URI."),
        std::string());
    return;
  }

  std::string session_id = base::NumberToString(next_session_id_++);
  auto session = std::make_unique<Session>();
  session->identifier = identifier;
  NSString* key = base::SysUTF8ToNSString(identifier);
  if (AVContentKeyRequest* request = pending_requests_[key]) {
    session->request = request;
    [pending_requests_ removeObjectForKey:key];
  }
  bool has_request = session->request != nil;
  sessions_[session_id] = std::move(session);
  std::move(callback).Run(Success(), session_id);
  // Else the license is requested when AVPlayer asks for this key.
  if (has_request) {
    RequestLicense(session_id, /*renewal=*/false);
  }
}

void AVFCdm::LoadSession(media::CdmSessionType session_type,
                         const std::string& session_id,
                         LoadSessionCallback callback) {
  std::move(callback).Run(Failure(Exception::NOT_SUPPORTED_ERROR,
                                  "Persistent sessions aren't supported."),
                          std::string());
}

void AVFCdm::UpdateSession(const std::string& session_id,
                           const std::vector<uint8_t>& response,
                           UpdateSessionCallback callback) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    std::move(callback).Run(
        Failure(Exception::INVALID_STATE_ERROR, "No such session."));
    return;
  }
  Session& session = *it->second;
  if (!session.request || !session.license_requested) {
    std::move(callback).Run(Failure(Exception::INVALID_STATE_ERROR,
                                    "No license was requested yet."));
    return;
  }
  if (response.empty()) {
    std::move(callback).Run(Failure(Exception::TYPE_ERROR, "Empty response."));
    return;
  }
  // The CKC. AVFoundation reports the outcome to the key session's delegate
  // (OnKeyRequestSucceeded() / OnKeyRequestFailed()).
  [session.request
      processContentKeyResponse:
          [AVContentKeyResponse
              contentKeyResponseWithFairPlayStreamingKeyResponseData:ToNSData(
                                                                         response)]];
  std::move(callback).Run(Success());
}

void AVFCdm::CloseSession(const std::string& session_id,
                          CloseSessionCallback callback) {
  if (sessions_.erase(session_id)) {
    if (client_) {
      client_->OnSessionClosed(session_id,
                               media::CdmSessionClosedReason::kClose);
    }
  }
  std::move(callback).Run(Success());
}

void AVFCdm::RemoveSession(const std::string& session_id,
                           RemoveSessionCallback callback) {
  std::move(callback).Run(Failure(Exception::NOT_SUPPORTED_ERROR,
                                  "Only temporary sessions are supported."));
}

void AVFCdm::OnKeyRequest(AVContentKeyRequest* request, bool renewal) {
  std::string identifier = IdentifierOf(request);
  if (identifier.empty()) {
    return;
  }
  // The session that has (or waits for) this key takes it: a first request,
  // AVFoundation's retry after a failure, or a renewal.
  for (auto& [session_id, session] : sessions_) {
    if (session->identifier == identifier) {
      session->request = request;
      session->license_requested = false;
      RequestLicense(session_id, renewal);
      return;
    }
  }
  pending_requests_[base::SysUTF8ToNSString(identifier)] = request;
}

void AVFCdm::RequestLicense(const std::string& session_id, bool renewal) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end() || !it->second->request) {
    return;
  }
  Session& session = *it->second;
  session.license_requested = true;
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  auto weak = weak_factory_.GetWeakPtr();
  // A copy: a block captures a C++ reference (the parameter) by reference.
  std::string id = session_id;
  NSData* content_identifier = [NSData dataWithBytes:session.identifier.data()
                                              length:session.identifier.size()];
  [session.request
      makeStreamingContentKeyRequestDataForApp:ToNSData(certificate_)
                             contentIdentifier:content_identifier
                                       options:nil
                             completionHandler:^(NSData* spc, NSError* error) {
                               std::vector<uint8_t> bytes;
                               if (spc) {
                                 auto span = base::apple::NSDataToSpan(spc);
                                 bytes.assign(span.begin(), span.end());
                               }
                               std::string message =
                                   error ? base::SysNSStringToUTF8(
                                               error.localizedDescription)
                                         : std::string();
                               task_runner->PostTask(
                                   FROM_HERE,
                                   base::BindOnce(&AVFCdm::OnLicenseRequestData,
                                                  weak, id, renewal,
                                                  std::move(bytes), message));
                             }];
}

void AVFCdm::OnLicenseRequestData(const std::string& session_id,
                                  bool renewal,
                                  std::vector<uint8_t> spc,
                                  const std::string& error) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end() || !client_) {
    return;
  }
  if (spc.empty()) {
    LOG(ERROR) << "FairPlay: no SPC: " << error;
    SendKeyStatus(session_id, it->second->identifier,
                  media::CdmKeyInformation::INTERNAL_ERROR);
    return;
  }
  client_->OnSessionMessage(session_id,
                            renewal ? media::CdmMessageType::LICENSE_RENEWAL
                                    : media::CdmMessageType::LICENSE_REQUEST,
                            spc);
}

void AVFCdm::OnKeyRequestSucceeded(AVContentKeyRequest* request) {
  for (auto& [session_id, session] : sessions_) {
    if (session->request == request) {
      SendKeyStatus(session_id, session->identifier,
                    media::CdmKeyInformation::USABLE);
      return;
    }
  }
}

void AVFCdm::OnKeyRequestFailed(AVContentKeyRequest* request,
                                NSError* error) {
  LOG(ERROR) << "FairPlay key request failed: "
             << base::SysNSStringToUTF8(error.localizedDescription ?: @"");
  for (auto& [session_id, session] : sessions_) {
    if (session->request == request) {
      SendKeyStatus(session_id, session->identifier,
                    media::CdmKeyInformation::INTERNAL_ERROR);
      return;
    }
  }
}

void AVFCdm::SendKeyStatus(const std::string& session_id,
                           const std::string& identifier,
                           media::CdmKeyInformation::KeyStatus status) {
  if (!client_) {
    return;
  }
  std::vector<std::unique_ptr<media::CdmKeyInformation>> keys;
  keys.push_back(
      std::make_unique<media::CdmKeyInformation>(identifier, status, 0));
  client_->OnSessionKeysChange(
      session_id, status == media::CdmKeyInformation::USABLE, std::move(keys));
}

}  // namespace compass::avf
