// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/pairing.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "crypto/aead.h"
#include "crypto/hash.h"
#include "crypto/hmac.h"
#include "crypto/kdf.h"
#include "crypto/random.h"

namespace compass::sync {
namespace {

constexpr size_t kDigits = 6;
constexpr size_t kSpakeMessageSize = 32;
constexpr size_t kMacSize = 32;
constexpr size_t kNonceSize = 12;
constexpr uint8_t kAllow = 0x01;
constexpr uint8_t kDeny = 0x00;
constexpr char kAdFromA[] = "compass-pair-a";
constexpr char kAdFromB[] = "compass-pair-b";
// Long polls that fail without a reply are retried after this.
constexpr base::TimeDelta kRetryDelay = base::Seconds(2);

constexpr int kHttpOk = 200;
constexpr int kHttpCreated = 201;
constexpr int kHttpNotFound = 404;
constexpr int kHttpConflict = 409;
constexpr int kHttpGone = 410;
constexpr int kHttpTooManyRequests = 429;
constexpr int kHttpUnavailable = 503;

bool IsSuccess(int status) {
  return status == kHttpOk || status == kHttpCreated;
}

// A uniformly random 6-digit string.
std::string RandomDigits() {
  // Rejection sampling: the largest multiple of 10^6 that fits in 32 bits.
  constexpr uint32_t kRange = 1000000;
  constexpr uint32_t kLimit = (0xffffffffu / kRange) * kRange;
  uint32_t value;
  do {
    std::array<uint8_t, 4> bytes = crypto::RandBytesAsArray<4>();
    value = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
            (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
  } while (value >= kLimit);
  std::string digits = base::NumberToString(value % kRange);
  return std::string(kDigits - digits.size(), '0') + digits;
}

bssl::UniquePtr<SPAKE2_CTX> NewSpake(PairingRole role,
                                     std::string_view host,
                                     int slot) {
  const std::string mine = PairingSpakeName(role, host, slot);
  const std::string theirs = PairingSpakeName(
      role == PairingRole::kA ? PairingRole::kB : PairingRole::kA, host, slot);
  return bssl::UniquePtr<SPAKE2_CTX>(SPAKE2_CTX_new(
      role == PairingRole::kA ? spake2_role_alice : spake2_role_bob,
      reinterpret_cast<const uint8_t*>(mine.data()), mine.size(),
      reinterpret_cast<const uint8_t*>(theirs.data()), theirs.size()));
}

// Our SPAKE2 message for |digits|, or empty on failure.
std::vector<uint8_t> GenerateSpakeMessage(SPAKE2_CTX* ctx,
                                          std::string_view digits) {
  std::vector<uint8_t> message(SPAKE2_MAX_MSG_SIZE);
  size_t length = 0;
  if (!ctx ||
      !SPAKE2_generate_msg(ctx, message.data(), &length, message.size(),
                           reinterpret_cast<const uint8_t*>(digits.data()),
                           digits.size())) {
    return {};
  }
  message.resize(length);
  return message;
}

// The pairing keys from the peer's SPAKE2 message, or nullopt.
std::optional<PairingKeys> ProcessSpakeMessage(
    SPAKE2_CTX* ctx,
    base::span<const uint8_t> their_message) {
  std::array<uint8_t, SPAKE2_MAX_KEY_SIZE> key{};
  size_t key_length = 0;
  if (their_message.size() != kSpakeMessageSize ||
      !SPAKE2_process_msg(ctx, key.data(), &key_length, key.size(),
                          their_message.data(), their_message.size())) {
    return std::nullopt;
  }
  return DerivePairingKeys(base::span(key).first(key_length));
}

// nonce || AES-256-GCM(key, plaintext, ad).
std::vector<uint8_t> Seal(const std::array<uint8_t, 32>& key,
                          base::span<const uint8_t> plaintext,
                          std::string_view ad) {
  const auto nonce = crypto::RandBytesAsArray<kNonceSize>();
  std::vector<uint8_t> sealed = crypto::aead::Seal(
      crypto::aead::AES_256_GCM, key, plaintext, nonce, base::as_byte_span(ad));
  std::vector<uint8_t> out(nonce.begin(), nonce.end());
  out.insert(out.end(), sealed.begin(), sealed.end());
  return out;
}

std::optional<std::vector<uint8_t>> Open(const std::array<uint8_t, 32>& key,
                                         base::span<const uint8_t> data,
                                         std::string_view ad) {
  if (data.size() < kNonceSize) {
    return std::nullopt;
  }
  return crypto::aead::Open(crypto::aead::AES_256_GCM, key,
                            data.subspan(kNonceSize), data.first(kNonceSize),
                            base::as_byte_span(ad));
}

bool VerifyMac(const PairingKeys& keys,
               PairingRole sender,
               base::span<const uint8_t> mac) {
  if (mac.size() != kMacSize) {
    return false;
  }
  const std::array<uint8_t, 32>& key =
      sender == PairingRole::kA ? keys.confirm_a : keys.confirm_b;
  const std::string label = sender == PairingRole::kA
                                ? "compass-pair-confirm-a"
                                : "compass-pair-confirm-b";
  return crypto::hmac::VerifySha256(key, base::as_byte_span(label),
                                    mac.first<kMacSize>());
}

void Append(std::vector<uint8_t>& out, base::span<const uint8_t> data) {
  out.insert(out.end(), data.begin(), data.end());
}

// What A makes of a slot the server says closed.
PairingHost::Result HostResultForClosed(PairingClosedReason closed) {
  switch (closed) {
    case PairingClosedReason::kMismatch:
      return PairingHost::Result::kWrongCode;
    case PairingClosedReason::kCancel:
      return PairingHost::Result::kCanceled;
    case PairingClosedReason::kDeny:
      return PairingHost::Result::kDenied;
    case PairingClosedReason::kExpired:
    case PairingClosedReason::kUnknown:
      return PairingHost::Result::kExpired;
  }
  return PairingHost::Result::kExpired;
}

// What B makes of it.
PairingJoiner::Result JoinerResultForClosed(PairingClosedReason closed) {
  switch (closed) {
    case PairingClosedReason::kMismatch:
      return PairingJoiner::Result::kWrongCode;
    case PairingClosedReason::kCancel:
      return PairingJoiner::Result::kCanceled;
    case PairingClosedReason::kDeny:
      return PairingJoiner::Result::kDenied;
    case PairingClosedReason::kExpired:
      return PairingJoiner::Result::kExpired;
    case PairingClosedReason::kUnknown:
      return PairingJoiner::Result::kClosed;
  }
  return PairingJoiner::Result::kClosed;
}

}  // namespace

std::string_view PairingClosedReasonName(PairingClosedReason reason) {
  switch (reason) {
    case PairingClosedReason::kMismatch:
      return "mismatch";
    case PairingClosedReason::kCancel:
      return "cancel";
    case PairingClosedReason::kDeny:
      return "deny";
    case PairingClosedReason::kExpired:
      return "expired";
    case PairingClosedReason::kUnknown:
      return std::string_view();
  }
  return std::string_view();
}

PairingClosedReason ParsePairingClosedReason(std::string_view name) {
  for (PairingClosedReason reason :
       {PairingClosedReason::kMismatch, PairingClosedReason::kCancel,
        PairingClosedReason::kDeny, PairingClosedReason::kExpired}) {
    if (name == PairingClosedReasonName(reason)) {
      return reason;
    }
  }
  return PairingClosedReason::kUnknown;
}

std::optional<PairingCode> ParsePairingCode(std::string_view text) {
  std::string code;
  base::RemoveChars(base::TrimWhitespaceASCII(text, base::TRIM_ALL), " ",
                    &code);
  const size_t dash = code.find('-');
  if (dash == std::string::npos || dash == 0) {
    return std::nullopt;
  }
  const std::string_view slot_text = std::string_view(code).substr(0, dash);
  const std::string_view digits = std::string_view(code).substr(dash + 1);
  int slot = 0;
  if (!std::ranges::all_of(slot_text, base::IsAsciiDigit<char>) ||
      !base::StringToInt(slot_text, &slot) || slot < 1 ||
      digits.size() != kDigits ||
      !std::ranges::all_of(digits, base::IsAsciiDigit<char>)) {
    return std::nullopt;
  }
  return PairingCode{slot, std::string(digits)};
}

std::string FormatPairingCode(const PairingCode& code) {
  return base::StrCat({base::NumberToString(code.slot), "-", code.digits});
}

PairingKeys DerivePairingKeys(base::span<const uint8_t> spake2_key) {
  auto derive = [&](std::string_view info) {
    return crypto::kdf::Hkdf<32>(crypto::hash::kSha256, spake2_key,
                                 /*salt=*/base::span<const uint8_t>(),
                                 base::as_byte_span(info));
  };
  PairingKeys keys;
  keys.enc = derive("compass-pair-enc");
  keys.confirm_a = derive("compass-pair-confirm-a");
  keys.confirm_b = derive("compass-pair-confirm-b");
  return keys;
}

std::array<uint8_t, 32> PairingConfirmMac(const PairingKeys& keys,
                                          PairingRole sender) {
  return sender == PairingRole::kA
             ? crypto::hmac::SignSha256(keys.confirm_a,
                                        base::as_byte_span(std::string_view(
                                            "compass-pair-confirm-a")))
             : crypto::hmac::SignSha256(keys.confirm_b,
                                        base::as_byte_span(std::string_view(
                                            "compass-pair-confirm-b")));
}

std::string PairingSpakeName(PairingRole role,
                             std::string_view host,
                             int slot) {
  return base::StrCat({role == PairingRole::kA ? "compass-a:" : "compass-b:",
                       host, ":", base::NumberToString(slot)});
}

// PairingHost ---------------------------------------------------------------

PairingHost::PairingHost(std::unique_ptr<PairingTransport> transport,
                         std::string host,
                         const Seed& seed,
                         Delegate* delegate)
    : transport_(std::move(transport)),
      host_(std::move(host)),
      seed_(seed),
      delegate_(delegate) {}

PairingHost::~PairingHost() {
  if (slot_open_) {
    transport_->Close(slot_, PairingRole::kA, PairingClosedReason::kCancel);
  }
}

void PairingHost::Start() {
  CHECK_EQ(step_, Step::kIdle);
  step_ = Step::kOpening;
  transport_->Open(
      base::BindOnce(&PairingHost::OnOpened, weak_factory_.GetWeakPtr()));
}

void PairingHost::OnOpened(int status,
                           int slot,
                           base::TimeDelta expires_in,
                           std::string server) {
  if (status != kHttpCreated || slot < 1) {
    // 429: too many opens from this IP; 503: the server's slots are full.
    // Both mean "try again later".
    Finish(status == kHttpTooManyRequests || status == kHttpUnavailable
               ? Result::kRateLimited
               : Result::kFailed,
           /*close=*/false);
    return;
  }
  slot_ = slot;
  slot_open_ = true;
  // The server's own name, so both sides bind the same one however each
  // reaches it (an IP, a Tailscale name).
  if (!server.empty()) {
    host_ = std::move(server);
  }
  const PairingCode code{slot, RandomDigits()};
  spake_ = NewSpake(PairingRole::kA, host_, slot);
  alice_message_ = GenerateSpakeMessage(spake_.get(), code.digits);
  if (alice_message_.empty()) {
    Finish(Result::kFailed, /*close=*/true);
    return;
  }
  step_ = Step::kWaitingForB;
  expiry_timer_.Start(
      FROM_HERE, expires_in,
      base::BindOnce(&PairingHost::OnExpired, base::Unretained(this)));
  delegate_->OnPairingCode(FormatPairingCode(code), expires_in);
  Poll();
}

void PairingHost::Poll() {
  transport_->Receive(
      slot_, PairingRole::kA, last_seq_,
      base::BindOnce(&PairingHost::OnReceived, weak_factory_.GetWeakPtr()));
}

void PairingHost::OnReceived(int status,
                             PairingClosedReason closed,
                             std::vector<PairingMessage> messages) {
  if (step_ != Step::kWaitingForB && step_ != Step::kWaitingForConfirmation) {
    return;
  }
  if (status == 0) {
    retry_timer_.Start(
        FROM_HERE, kRetryDelay,
        base::BindOnce(&PairingHost::Poll, base::Unretained(this)));
    return;
  }
  if (status == kHttpGone) {
    slot_open_ = false;
    Finish(HostResultForClosed(closed), /*close=*/false);
    return;
  }
  if (status == kHttpNotFound) {
    Finish(Result::kExpired, /*close=*/false);
    return;
  }
  if (!IsSuccess(status)) {
    Finish(Result::kFailed, /*close=*/true);
    return;
  }
  std::ranges::sort(messages, {}, &PairingMessage::seq);
  for (const PairingMessage& message : messages) {
    if (message.seq <= last_seq_) {
      continue;
    }
    last_seq_ = message.seq;
    if (!HandleMessage(message)) {
      return;
    }
  }
  if (step_ == Step::kWaitingForB || step_ == Step::kWaitingForConfirmation) {
    Poll();
  }
}

bool PairingHost::HandleMessage(const PairingMessage& message) {
  switch (step_) {
    case Step::kWaitingForB: {
      // 1. B's SPAKE2 message.
      keys_ = ProcessSpakeMessage(spake_.get(), message.data);
      spake_.reset();
      if (!keys_) {
        Finish(Result::kWrongCode, /*close=*/true);
        return false;
      }
      step_ = Step::kWaitingForConfirmation;
      // 2. Ours. Polling goes on meanwhile; B answers only once it has it.
      transport_->Send(slot_, PairingRole::kA, alice_message_,
                       base::BindOnce(&PairingHost::OnAlicePosted,
                                      weak_factory_.GetWeakPtr()));
      return true;
    }
    case Step::kWaitingForConfirmation: {
      // 3. MAC_b || sealed device name.
      const base::span<const uint8_t> data(message.data);
      if (data.size() < kMacSize ||
          !VerifyMac(*keys_, PairingRole::kB, data.first(kMacSize))) {
        Finish(Result::kWrongCode, /*close=*/true);
        return false;
      }
      std::optional<std::vector<uint8_t>> name =
          Open(keys_->enc, data.subspan(kMacSize), kAdFromB);
      if (!name) {
        Finish(Result::kFailed, /*close=*/true);
        return false;
      }
      step_ = Step::kAsking;
      delegate_->OnPairingRequest(std::string(name->begin(), name->end()));
      return false;
    }
    default:
      return true;
  }
}

void PairingHost::OnAlicePosted(int status, PairingClosedReason closed) {
  if (step_ != Step::kWaitingForConfirmation) {
    return;
  }
  if (status == kHttpGone) {
    slot_open_ = false;
    Finish(HostResultForClosed(closed), /*close=*/false);
  } else if (status == kHttpNotFound) {
    Finish(Result::kExpired, /*close=*/false);
  } else if (!IsSuccess(status)) {
    Finish(
        status == kHttpTooManyRequests ? Result::kRateLimited : Result::kFailed,
        /*close=*/true);
  }
}

void PairingHost::Answer(bool allow) {
  if (step_ != Step::kAsking) {
    return;
  }
  step_ = Step::kAnswering;
  // 4. MAC_a || verdict [|| sealed seed].
  std::vector<uint8_t> message;
  Append(message, PairingConfirmMac(*keys_, PairingRole::kA));
  message.push_back(allow ? kAllow : kDeny);
  if (allow) {
    base::DictValue payload;
    payload.Set("seed", base::Base64Encode(seed_));
    payload.Set("profiles", base::ListValue());
    std::optional<std::string> json =
        base::WriteJson(base::Value(std::move(payload)));
    CHECK(json);
    Append(message, Seal(keys_->enc, base::as_byte_span(*json), kAdFromA));
  }
  transport_->Send(slot_, PairingRole::kA, std::move(message),
                   base::BindOnce(&PairingHost::OnAnswerPosted,
                                  weak_factory_.GetWeakPtr(), allow));
}

void PairingHost::OnAnswerPosted(bool allow,
                                 int status,
                                 PairingClosedReason closed) {
  if (status == kHttpGone) {
    slot_open_ = false;
    Finish(HostResultForClosed(closed), /*close=*/false);
    return;
  }
  if (!IsSuccess(status)) {
    Finish(status == kHttpNotFound ? Result::kExpired : Result::kFailed,
           /*close=*/status != kHttpNotFound);
    return;
  }
  if (allow) {
    // B reads the answer, then closes the slot; it expires anyway.
    slot_open_ = false;
    Finish(Result::kAdded, /*close=*/false);
    return;
  }
  // Denied: B learns it from the answer or, if the slot closes first, from
  // the server ("deny").
  slot_open_ = false;
  transport_->Close(slot_, PairingRole::kA, PairingClosedReason::kDeny);
  Finish(Result::kDenied, /*close=*/false);
}

void PairingHost::OnExpired() {
  if (step_ == Step::kDone || step_ == Step::kAnswering) {
    return;
  }
  Finish(Result::kExpired, /*close=*/true);
}

void PairingHost::Finish(Result result, bool close) {
  if (step_ == Step::kDone) {
    return;
  }
  step_ = Step::kDone;
  expiry_timer_.Stop();
  retry_timer_.Stop();
  weak_factory_.InvalidateWeakPtrs();
  spake_.reset();
  keys_.reset();
  if (close && slot_open_) {
    transport_->Close(slot_, PairingRole::kA,
                      result == Result::kWrongCode
                          ? PairingClosedReason::kMismatch
                          : PairingClosedReason::kCancel);
  }
  slot_open_ = false;
  delegate_->OnPairingFinished(result);
}

// PairingJoiner -------------------------------------------------------------

PairingJoiner::PairingJoiner(std::unique_ptr<PairingTransport> transport,
                             std::string host,
                             std::string device_name)
    : transport_(std::move(transport)),
      host_(std::move(host)),
      device_name_(std::move(device_name)) {}

PairingJoiner::~PairingJoiner() {
  if (joined_ && step_ != Step::kDone) {
    transport_->Close(code_.slot, PairingRole::kB,
                      PairingClosedReason::kCancel);
  }
}

void PairingJoiner::Start(std::string_view code,
                          base::OnceClosure waiting,
                          DoneCallback done) {
  CHECK_EQ(step_, Step::kIdle);
  waiting_ = std::move(waiting);
  done_ = std::move(done);
  std::optional<PairingCode> parsed = ParsePairingCode(code);
  if (!parsed) {
    // Asynchronously, as every other result.
    step_ = Step::kJoining;
    retry_timer_.Start(
        FROM_HERE, base::TimeDelta(),
        base::BindOnce(&PairingJoiner::Finish, base::Unretained(this),
                       Result::kBadCode, std::nullopt, /*close=*/false));
    return;
  }
  code_ = *parsed;
  step_ = Step::kJoining;
  timeout_timer_.Start(
      FROM_HERE, kTimeout,
      base::BindOnce(&PairingJoiner::OnTimeout, base::Unretained(this)));
  transport_->Join(code_.slot, base::BindOnce(&PairingJoiner::OnJoined,
                                              weak_factory_.GetWeakPtr()));
}

void PairingJoiner::OnJoined(int status,
                             PairingClosedReason closed,
                             std::string server,
                             base::TimeDelta expires_in) {
  if (!IsSuccess(status)) {
    Result result = Result::kFailed;
    if (status == kHttpGone) {
      // The code was right but its slot closed: A canceled, or it expired.
      result = JoinerResultForClosed(closed);
    } else if (status == kHttpNotFound) {
      result = Result::kNoSuchCode;
    } else if (status == kHttpConflict) {
      result = Result::kCodeUsed;
    } else if (status == kHttpTooManyRequests) {
      result = Result::kRateLimited;
    }
    Finish(result, std::nullopt, /*close=*/false);
    return;
  }
  joined_ = true;
  if (!server.empty()) {
    host_ = std::move(server);
  }
  if (expires_in.is_positive()) {
    // Wait as long as the slot lives, not a fixed 5.5 minutes.
    timeout_timer_.Start(
        FROM_HERE, expires_in + kTimeoutMargin,
        base::BindOnce(&PairingJoiner::OnTimeout, base::Unretained(this)));
  }
  spake_ = NewSpake(PairingRole::kB, host_, code_.slot);
  std::vector<uint8_t> bob = GenerateSpakeMessage(spake_.get(), code_.digits);
  if (bob.empty()) {
    Finish(Result::kFailed, std::nullopt, /*close=*/true);
    return;
  }
  step_ = Step::kSendingBob;
  // 1. Our SPAKE2 message.
  transport_->Send(
      code_.slot, PairingRole::kB, std::move(bob),
      base::BindOnce(&PairingJoiner::OnBobPosted, weak_factory_.GetWeakPtr()));
}

void PairingJoiner::OnBobPosted(int status, PairingClosedReason closed) {
  if (status == kHttpGone) {
    Finish(JoinerResultForClosed(closed), std::nullopt, /*close=*/false);
    return;
  }
  if (!IsSuccess(status)) {
    Finish(status == kHttpNotFound ? Result::kClosed : Result::kFailed,
           std::nullopt, /*close=*/status != kHttpNotFound);
    return;
  }
  step_ = Step::kWaitingForAlice;
  Poll();
}

void PairingJoiner::Poll() {
  transport_->Receive(
      code_.slot, PairingRole::kB, last_seq_,
      base::BindOnce(&PairingJoiner::OnReceived, weak_factory_.GetWeakPtr()));
}

void PairingJoiner::OnReceived(int status,
                               PairingClosedReason closed,
                               std::vector<PairingMessage> messages) {
  if (step_ != Step::kWaitingForAlice && step_ != Step::kWaitingForAnswer) {
    return;
  }
  if (status == 0) {
    retry_timer_.Start(
        FROM_HERE, kRetryDelay,
        base::BindOnce(&PairingJoiner::Poll, base::Unretained(this)));
    return;
  }
  if (status == kHttpGone) {
    Finish(JoinerResultForClosed(closed), std::nullopt, /*close=*/false);
    return;
  }
  if (status == kHttpNotFound) {
    Finish(Result::kClosed, std::nullopt, /*close=*/false);
    return;
  }
  if (!IsSuccess(status)) {
    Finish(Result::kFailed, std::nullopt, /*close=*/true);
    return;
  }
  std::ranges::sort(messages, {}, &PairingMessage::seq);
  for (const PairingMessage& message : messages) {
    if (message.seq <= last_seq_) {
      continue;
    }
    last_seq_ = message.seq;
    if (!HandleMessage(message)) {
      return;
    }
  }
  if (step_ == Step::kWaitingForAlice || step_ == Step::kWaitingForAnswer) {
    Poll();
  }
}

bool PairingJoiner::HandleMessage(const PairingMessage& message) {
  switch (step_) {
    case Step::kWaitingForAlice: {
      // 2. A's SPAKE2 message.
      keys_ = ProcessSpakeMessage(spake_.get(), message.data);
      spake_.reset();
      if (!keys_) {
        Finish(Result::kFailed, std::nullopt, /*close=*/true);
        return false;
      }
      // 3. MAC_b || sealed device name.
      std::vector<uint8_t> confirmation;
      Append(confirmation, PairingConfirmMac(*keys_, PairingRole::kB));
      Append(confirmation,
             Seal(keys_->enc, base::as_byte_span(device_name_), kAdFromB));
      step_ = Step::kSendingConfirmation;
      transport_->Send(code_.slot, PairingRole::kB, std::move(confirmation),
                       base::BindOnce(&PairingJoiner::OnConfirmationPosted,
                                      weak_factory_.GetWeakPtr()));
      return false;
    }
    case Step::kWaitingForAnswer: {
      // 4. MAC_a || verdict [|| sealed seed].
      const base::span<const uint8_t> data(message.data);
      if (data.size() < kMacSize + 1 ||
          !VerifyMac(*keys_, PairingRole::kA, data.first(kMacSize))) {
        Finish(Result::kWrongCode, std::nullopt, /*close=*/true);
        return false;
      }
      const uint8_t verdict = data[kMacSize];
      if (verdict != kAllow) {
        Finish(Result::kDenied, std::nullopt, /*close=*/true);
        return false;
      }
      std::optional<std::vector<uint8_t>> json =
          Open(keys_->enc, data.subspan(kMacSize + 1), kAdFromA);
      std::optional<base::DictValue> payload;
      if (json) {
        payload = base::JSONReader::ReadDict(
            std::string_view(reinterpret_cast<const char*>(json->data()),
                             json->size()),
            base::JSON_PARSE_RFC);
      }
      const std::string* seed_text =
          payload ? payload->FindString("seed") : nullptr;
      std::optional<std::vector<uint8_t>> seed_bytes =
          seed_text ? base::Base64Decode(*seed_text) : std::nullopt;
      if (!seed_bytes || seed_bytes->size() != kSeedSize) {
        Finish(Result::kFailed, std::nullopt, /*close=*/true);
        return false;
      }
      Seed seed;
      std::ranges::copy(*seed_bytes, seed.begin());
      Finish(Result::kJoined, seed, /*close=*/true);
      return false;
    }
    default:
      return true;
  }
}

void PairingJoiner::OnConfirmationPosted(int status,
                                         PairingClosedReason closed) {
  if (status == kHttpGone) {
    Finish(JoinerResultForClosed(closed), std::nullopt, /*close=*/false);
    return;
  }
  if (!IsSuccess(status)) {
    Finish(status == kHttpNotFound ? Result::kClosed : Result::kFailed,
           std::nullopt, /*close=*/status != kHttpNotFound);
    return;
  }
  step_ = Step::kWaitingForAnswer;
  if (waiting_) {
    std::move(waiting_).Run();
  }
  Poll();
}

void PairingJoiner::OnTimeout() {
  Finish(Result::kExpired, std::nullopt, /*close=*/true);
}

void PairingJoiner::Finish(Result result,
                           std::optional<Seed> seed,
                           bool close) {
  if (step_ == Step::kDone) {
    return;
  }
  step_ = Step::kDone;
  timeout_timer_.Stop();
  retry_timer_.Stop();
  weak_factory_.InvalidateWeakPtrs();
  spake_.reset();
  keys_.reset();
  if (close && joined_) {
    transport_->Close(code_.slot, PairingRole::kB,
                      result == Result::kWrongCode
                          ? PairingClosedReason::kMismatch
                          : PairingClosedReason::kCancel);
  }
  if (done_) {
    std::move(done_).Run(result, seed);
  }
}

}  // namespace compass::sync
