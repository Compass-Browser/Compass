// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/avf_cdm.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <optional>
#include <utility>

#include "base/apple/foundation_util.h"
#include "compass/media/avf/service/fps_protocol.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/cdm_key_information.h"
#include "media/base/cdm_promise.h"
#include "media/base/content_decryption_module.h"

// The CDM's key session's delegate, on the main thread.
@interface CompassAVFCdmKeyDelegate : NSObject <AVContentKeySessionDelegate>
- (instancetype)initWithOwner:(base::WeakPtr<compass::avf::AVFCdm>)owner;
@end

@implementation CompassAVFCdmKeyDelegate {
  base::WeakPtr<compass::avf::AVFCdm> _owner;
}

- (instancetype)initWithOwner:(base::WeakPtr<compass::avf::AVFCdm>)owner {
  if ((self = [super init])) {
    _owner = owner;
  }
  return self;
}

- (void)contentKeySession:(AVContentKeySession*)session
    didProvideContentKeyRequest:(AVContentKeyRequest*)keyRequest {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequest(keyRequest, /*renewal=*/false);
  }
}

- (void)contentKeySession:(AVContentKeySession*)session
    didProvideRenewingContentKeyRequest:(AVContentKeyRequest*)keyRequest {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequest(keyRequest, /*renewal=*/true);
  }
}

- (void)contentKeySession:(AVContentKeySession*)session
    contentKeyRequestDidSucceed:(AVContentKeyRequest*)keyRequest {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequestSucceeded(keyRequest);
  }
}

- (void)contentKeySession:(AVContentKeySession*)session
           contentKeyRequest:(AVContentKeyRequest*)keyRequest
    didFailWithError:(NSError*)error {
  if (auto* owner = _owner.get()) {
    owner->OnKeyRequestFailed(keyRequest, error);
  }
}

@end

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
    // A request made from FairPlay 'pssh' init data is named by its key ID;
    // the skd:// URI MSE players use for it (KeyUri()).
    if (bytes.size() == 16) {
      return "skd://" + base::ToLowerASCII(base::HexEncode(bytes));
    }
    return std::string(bytes.begin(), bytes.end());
  }
  return std::string();
}

NSData* ToNSData(base::span<const uint8_t> bytes) {
  return [NSData dataWithBytes:bytes.data() length:bytes.size()];
}

// The key IDs in 'cenc' init data: one or more 'pssh' boxes; version 1 boxes
// list their keys.
std::optional<std::vector<std::string>> KeyIdsFromPssh(
    const std::vector<uint8_t>& data) {
  std::vector<std::string> key_ids;
  size_t offset = 0;
  auto u32 = [&](size_t at) {
    return (static_cast<uint32_t>(data[at]) << 24) |
           (static_cast<uint32_t>(data[at + 1]) << 16) |
           (static_cast<uint32_t>(data[at + 2]) << 8) | data[at + 3];
  };
  while (offset < data.size()) {
    if (offset + 32 > data.size()) {
      return std::nullopt;
    }
    uint32_t size = u32(offset);
    if (size < 32 || offset + size > data.size() ||
        u32(offset + 4) != 0x70737368) {  // 'pssh'
      return std::nullopt;
    }
    uint8_t version = data[offset + 8];
    size_t position = offset + 28;  // After the 16-byte system ID.
    if (version > 0) {
      uint32_t count = u32(position);
      position += 4;
      if (position + 16ull * count > offset + size) {
        return std::nullopt;
      }
      for (uint32_t i = 0; i < count; ++i) {
        std::string key_id(data.begin() + position,
                           data.begin() + position + 16);
        if (std::find(key_ids.begin(), key_ids.end(), key_id) ==
            key_ids.end()) {
          key_ids.push_back(std::move(key_id));
        }
        position += 16;
      }
    }
    offset += size;
  }
  return key_ids;
}

// The skd:// URI the MSE player's playlist names a key by
// (mse::HlsSession): the key ID in hex.
std::string KeyUri(const std::string& key_id) {
  return "skd://" + base::ToLowerASCII(base::HexEncode(key_id));
}

}  // namespace

struct AVFCdm::Session {
  // Safari's FairPlay protocol (fps_protocol.h): keys named by a FairPlay
  // 'pssh'; the page asks for their SPCs ('kreq') and returns CKCs ('ckcs').
  struct FpsKey {
    std::string key_id;
    std::vector<uint8_t> asset_id;
    AVContentKeyRequest* __strong request;
    bool wanted = false;  // The page asked for its SPC.
    std::vector<uint32_t> versions;
    // The last CKC the page gave for this key.
    std::vector<uint8_t> ckc;
  };
  bool fps = false;
  FpsFormat fps_format = FpsFormat::kBinary;
  std::vector<FpsKey> fps_keys;

  FpsKey* FpsKeyFor(const std::string& uri) {
    for (auto& key : fps_keys) {
      if (KeyUri(key.key_id) == uri) {
        return &key;
      }
    }
    return nullptr;
  }

  // skd:// init data: the URI. 'cenc' (MSE): the URIs of its key IDs; none
  // if the init data names no key, and then the first key request it gets.
  std::vector<std::string> identifiers;
  bool cenc = false;
  // The request's identifier, once it has one.
  std::string identifier;
  AVContentKeyRequest* __strong request;
  bool license_requested = false;

  bool Wants(const std::string& key) const {
    if (!identifier.empty()) {
      return identifier == key;
    }
    return identifiers.empty() ||
           std::find(identifiers.begin(), identifiers.end(), key) !=
               identifiers.end();
  }

  // What makeStreamingContentKeyRequestDataForApp gets: the skd:// URI, as
  // Safari for HLS; the key ID for 'cenc', as Safari for MSE.
  NSData* ContentIdentifier() const {
    if (cenc) {
      std::string key_id;
      if (base::HexStringToString(identifier.substr(6), &key_id)) {
        return [NSData dataWithBytes:key_id.data() length:key_id.size()];
      }
    }
    return [NSData dataWithBytes:identifier.data() length:identifier.size()];
  }

  // The key ID the page sees in keyStatuses.
  std::string KeyIdForStatus() const {
    std::string key_id;
    if (cenc && base::HexStringToString(identifier.substr(6), &key_id)) {
      return key_id;
    }
    return identifier;
  }
};

AVFCdm::AVFCdm(
    const base::UnguessableToken& id,
    mojo::PendingReceiver<media::mojom::ContentDecryptionModule> receiver,
    base::OnceClosure on_disconnect)
    : id_(id),
      receiver_(this, std::move(receiver)),
      on_disconnect_(std::move(on_disconnect)),
      pending_requests_([NSMutableDictionary dictionary]) {
  key_delegate_ = [[CompassAVFCdmKeyDelegate alloc]
      initWithOwner:weak_factory_.GetWeakPtr()];
  key_session_ = [AVContentKeySession
      contentKeySessionWithKeySystem:AVContentKeySystemFairPlayStreaming];
  [key_session_ setDelegate:key_delegate_ queue:dispatch_get_main_queue()];
  receiver_.set_disconnect_handler(base::BindOnce(
      [](AVFCdm* cdm) {
        if (cdm->on_disconnect_) {
          std::move(cdm->on_disconnect_).Run();  // Deletes `cdm`.
        }
      },
      base::Unretained(this)));
}

AVFCdm::~AVFCdm() {
  [key_session_ setDelegate:nil queue:nil];
}

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
  if (init_data_type != media::EmeInitDataType::SKD &&
      init_data_type != media::EmeInitDataType::CENC) {
    std::move(callback).Run(
        Failure(Exception::NOT_SUPPORTED_ERROR,
                "Only 'skd' and 'cenc' init data are supported."),
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
  auto session = std::make_unique<Session>();
  if (init_data_type == media::EmeInitDataType::SKD) {
    std::string identifier(init_data.begin(), init_data.end());
    if (!identifier.starts_with("skd://")) {
      std::move(callback).Run(Failure(Exception::TYPE_ERROR,
                                      "'skd' init data must be an skd:// URI."),
                              std::string());
      return;
    }
    session->identifiers.push_back(identifier);
  } else if (std::optional<FpsInitData> fps_init = ParseFpsInitData(init_data)) {
    // Safari's FairPlay protocol (Apple's hls.js).
    session->cenc = true;
    session->fps = true;
    session->fps_format = fps_init->format;
    std::vector<FpsKeyRequest>* fps_keys = &fps_init->keys;
    // The init data is the key request: the page waits for the SPCs ('kreq'
    // is for keys added to the session later).
    for (auto& key : *fps_keys) {
      session->identifiers.push_back(KeyUri(key.key_id));
      session->fps_keys.push_back({key.key_id, std::move(key.asset_id), nil,
                                   /*wanted=*/true, std::move(key.versions)});
    }
  } else {
    // FairPlay for MSE: AVPlayer asks for skd://<key ID>.
    std::optional<std::vector<std::string>> key_ids = KeyIdsFromPssh(init_data);
    if (!key_ids) {
      std::move(callback).Run(
          Failure(Exception::TYPE_ERROR, "Malformed 'cenc' init data."),
          std::string());
      return;
    }
    session->cenc = true;
    for (const auto& key_id : *key_ids) {
      session->identifiers.push_back(KeyUri(key_id));
    }
  }

  std::string session_id = base::NumberToString(next_session_id_++);
  VLOG(1) << "FairPlay: session " << session_id << " for "
          << (session->fps ? "Safari's protocol" : session->cenc ? "cenc" : "skd")
          << ", keys " << base::JoinString(session->identifiers, " ");
  if (session->fps) {
    std::vector<std::string> ready;
    for (NSString* key in pending_requests_.allKeys) {
      if (Session::FpsKey* fps_key =
              session->FpsKeyFor(base::SysNSStringToUTF8(key))) {
        fps_key->request = pending_requests_[key];
        [pending_requests_ removeObjectForKey:key];
        ready.push_back(fps_key->key_id);
      }
    }
    // The others: the page waits for their SPCs before it loads any media
    // (Apple TV+), so ask for them now, as Safari does; MSE players find
    // them in this key session.
    std::vector<std::string> to_request;
    for (const auto& key : session->fps_keys) {
      if (!key.request) {
        to_request.push_back(KeyUri(key.key_id));
      }
    }
    // By the skd:// URI MSE players' playlists name the key by
    // (mse::HlsSession), so their AVPlayer finds it loaded: a request made
    // from the 'pssh' (as WebKit does) is named by the key ID instead, and
    // AVPlayer would ask for a second license, which pages don't give.
    sessions_[session_id] = std::move(session);
    std::move(callback).Run(Success(), session_id);
    for (const auto& key_id : ready) {
      RequestFpsSpc(session_id, key_id, /*renewal=*/false);
    }
    for (const auto& uri : to_request) {
      [key_session_ processContentKeyRequestWithIdentifier:
                        base::SysUTF8ToNSString(uri)
                                        initializationData:nil
                                                   options:nil];
    }
    return;
  }
  for (NSString* key in pending_requests_.allKeys) {
    std::string pending = base::SysNSStringToUTF8(key);
    if (session->Wants(pending)) {
      session->identifier = pending;
      session->request = pending_requests_[key];
      [pending_requests_ removeObjectForKey:key];
      break;
    }
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
  if (session.fps) {
    UpdateFpsSession(session_id, response, std::move(callback));
    return;
  }
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

void AVFCdm::UpdateFpsSession(const std::string& session_id,
                              const std::vector<uint8_t>& message,
                              UpdateSessionCallback callback) {
  Session& session = *sessions_[session_id];
  if (std::string(message.begin(), message.end()) == "renew") {
    // Safari's: renew the session's keys; their SPCs come as renewals.
    for (const auto& key : session.fps_keys) {
      if (key.request) {
        [key_session_ renewExpiringResponseDataForContentKeyRequest:key.request];
      }
    }
    std::move(callback).Run(Success());
    return;
  }
  std::optional<std::vector<FpsLicense>> json_licenses =
      ParseFpsJsonLicenses(message);
  if (!json_licenses && session.fps_format == FpsFormat::kWebKit) {
    // A bare CKC, for the session's one key.
    Session::FpsKey* only = nullptr;
    for (auto& key : session.fps_keys) {
      if (key.request) {
        only = only ? nullptr : &key;
      }
    }
    if (only) {
      json_licenses = std::vector<FpsLicense>{{only->key_id, 0, message}};
    }
  }
  if (json_licenses) {
    for (const auto& license : *json_licenses) {
      Session::FpsKey* key = session.FpsKeyFor(KeyUri(license.key_id));
      if (key) {
        key->ckc = license.ckc;
      }
      if (key && key->request && !license.ckc.empty()) {
        [key->request
            processContentKeyResponse:
                [AVContentKeyResponse
                    contentKeyResponseWithFairPlayStreamingKeyResponseData:
                        ToNSData(license.ckc)]];
      }
    }
    std::move(callback).Run(Success());
    return;
  }
  if (std::optional<std::vector<FpsKeyRequest>> requests =
          ParseFpsKeyRequestMessage(message)) {
    std::vector<std::string> ready;
    for (auto& request : *requests) {
      Session::FpsKey* key = session.FpsKeyFor(KeyUri(request.key_id));
      if (!key) {
        session.identifiers.push_back(KeyUri(request.key_id));
        session.fps_keys.push_back({request.key_id, {}});
        key = &session.fps_keys.back();
      }
      if (!request.asset_id.empty()) {
        key->asset_id = std::move(request.asset_id);
      }
      key->wanted = true;
      if (key->request) {
        ready.push_back(key->key_id);
      }
    }
    std::move(callback).Run(Success());
    // Else when AVPlayer asks for the key.
    for (const auto& key_id : ready) {
      RequestFpsSpc(session_id, key_id, /*renewal=*/false);
    }
    return;
  }
  if (std::optional<std::vector<FpsLicense>> licenses =
          ParseFpsLicenseMessage(message)) {
    for (const auto& license : *licenses) {
      Session::FpsKey* key = session.FpsKeyFor(KeyUri(license.key_id));
      if (key && key->request && !license.ckc.empty()) {
        [key->request
            processContentKeyResponse:
                [AVContentKeyResponse
                    contentKeyResponseWithFairPlayStreamingKeyResponseData:
                        ToNSData(license.ckc)]];
      }
    }
    std::move(callback).Run(Success());
    return;
  }
  std::move(callback).Run(Failure(Exception::TYPE_ERROR,
                                  "Expected a 'kreq' or 'ckcs' message."));
}

void AVFCdm::RequestFpsSpc(const std::string& session_id,
                           const std::string& key_id,
                           bool renewal) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return;
  }
  Session::FpsKey* key = it->second->FpsKeyFor(KeyUri(key_id));
  if (!key || !key->request) {
    return;
  }
  // The asset ID the page names, as Safari does; else the key ID.
  NSData* content_identifier =
      !key->asset_id.empty()
          ? ToNSData(key->asset_id)
          : [NSData dataWithBytes:key_id.data() length:key_id.size()];
  NSDictionary* options = nil;
  if (!key->versions.empty()) {
    NSMutableArray* versions = [NSMutableArray array];
    for (uint32_t version : key->versions) {
      [versions addObject:@(version)];
    }
    options = @{AVContentKeyRequestProtocolVersionsKey : versions};
  }
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  auto weak = weak_factory_.GetWeakPtr();
  std::string id = session_id;  // Copies: the block outlives the parameters.
  std::string kid = key_id;
  [key->request
      makeStreamingContentKeyRequestDataForApp:ToNSData(certificate_)
                             contentIdentifier:content_identifier
                                       options:options
                             completionHandler:^(NSData* spc, NSError* error) {
                               std::vector<uint8_t> bytes;
                               if (spc) {
                                 auto span = base::apple::NSDataToSpan(spc);
                                 bytes.assign(span.begin(), span.end());
                               } else {
                                 LOG(ERROR) << "FairPlay: SPC failed: "
                                            << base::SysNSStringToUTF8(
                                                   error.description ?: @"");
                               }
                               task_runner->PostTask(
                                   FROM_HERE,
                                   base::BindOnce(&AVFCdm::OnFpsSpc, weak, id,
                                                  kid, renewal,
                                                  std::move(bytes)));
                             }];
}

void AVFCdm::OnFpsSpc(const std::string& session_id,
                      const std::string& key_id,
                      bool renewal,
                      std::vector<uint8_t> spc) {
  if (!sessions_.contains(session_id) || !client_) {
    return;
  }
  if (spc.empty()) {
    LOG(ERROR) << "FairPlay: no SPC";
    SendKeyStatus(session_id, key_id,
                  media::CdmKeyInformation::INTERNAL_ERROR);
    return;
  }
  VLOG(1) << "FairPlay: SPC for " << base::HexEncode(key_id) << ", "
          << spc.size() << " bytes";
  const Session& session = *sessions_[session_id];
  if (session.fps_format == FpsFormat::kWebKit) {
    // Safari's: a renewal is the bare SPC.
    client_->OnSessionMessage(
        session_id,
        renewal ? media::CdmMessageType::LICENSE_RENEWAL
                : media::CdmMessageType::LICENSE_REQUEST,
        renewal ? spc : MakeFpsJsonMessage(key_id, spc));
    return;
  }
  client_->OnSessionMessage(session_id, media::CdmMessageType::LICENSE_REQUEST,
                            MakeFpsSpcMessage(key_id, spc));
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
  VLOG(1) << "FairPlay: key request " << identifier
          << (renewal ? " (renewal)" : "");
  if (identifier.empty()) {
    return;
  }
  // Safari's protocol: the key's SPC once the page asked for it.
  for (auto& [session_id, session] : sessions_) {
    if (!session->fps) {
      continue;
    }
    if (Session::FpsKey* key = session->FpsKeyFor(identifier)) {
      key->request = request;
      if (key->wanted || renewal) {
        RequestFpsSpc(session_id, key->key_id, renewal);
      }
      return;
    }
  }
  // The session that has (or waits for) this key takes it: a first request,
  // AVFoundation's retry after a failure, or a renewal.
  // A session for exactly this key first, then one that takes any.
  for (bool any : {false, true}) {
    for (auto& [session_id, session] : sessions_) {
      bool exact = session->identifier == identifier ||
                   (session->identifier.empty() &&
                    std::find(session->identifiers.begin(),
                              session->identifiers.end(),
                              identifier) != session->identifiers.end());
      if (session->fps) {
        continue;
      }
      if (any ? session->Wants(identifier) : exact) {
        session->identifier = identifier;
        session->request = request;
        session->license_requested = false;
        RequestLicense(session_id, renewal);
        return;
      }
    }
  }
  VLOG(1) << "FairPlay: no session for " << identifier << " yet";
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
  NSData* content_identifier = session.ContentIdentifier();
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
    SendKeyStatus(session_id, it->second->KeyIdForStatus(),
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
    for (const auto& key : session->fps_keys) {
      if (key.request == request) {
        SendKeyStatus(session_id, key.key_id,
                      media::CdmKeyInformation::USABLE);
        return;
      }
    }
    if (session->request == request) {
      SendKeyStatus(session_id, session->KeyIdForStatus(),
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
    for (const auto& key : session->fps_keys) {
      if (key.request == request) {
        SendKeyStatus(session_id, key.key_id,
                      media::CdmKeyInformation::INTERNAL_ERROR);
        return;
      }
    }
    if (session->request == request) {
      SendKeyStatus(session_id, session->KeyIdForStatus(),
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
