// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_PAIRING_H_
#define COMPASS_COMPONENTS_SYNC_PAIRING_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "compass/components/sync/chain.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"

// Adding a device: a device already syncing (A) passes the chain's seed to a
// new one (B) through the server, protected by a short code that never
// reaches the server. See docs/design/sync.md, "Pairing" and "Pairing API".
//
// Wire format of the messages relayed through the slot (the server doesn't
// read them):
//   1. B -> A: B's SPAKE2 message (32 bytes).
//   2. A -> B: A's SPAKE2 message (32 bytes).
//   3. B -> A: MAC_b (32) || nonce (12) || AES-256-GCM(K_enc, device name,
//      ad="compass-pair-b").
//   4. A -> B: MAC_a (32) || verdict (1: 0x01 allow, 0x00 deny)
//      [allow only: || nonce (12) || AES-256-GCM(K_enc, JSON {"seed":
//      base64, "profiles": []}, ad="compass-pair-a")].
// MAC_x = HMAC-SHA256(K_confirm_x, "compass-pair-confirm-x"), and K_enc,
// K_confirm_a, K_confirm_b = HKDF-SHA256(SPAKE2 key, salt="",
// info="compass-pair-enc" / "compass-pair-confirm-a" /
// "compass-pair-confirm-b", 32 bytes).
namespace compass::sync {

enum class PairingRole { kA, kB };

// "7-482913": the slot number, a dash, then 6 digits (the secret).
struct PairingCode {
  int slot = 0;
  std::string digits;
};
std::optional<PairingCode> ParsePairingCode(std::string_view text);
std::string FormatPairingCode(const PairingCode& code);

struct PairingKeys {
  std::array<uint8_t, 32> enc{};
  std::array<uint8_t, 32> confirm_a{};
  std::array<uint8_t, 32> confirm_b{};
};
PairingKeys DerivePairingKeys(base::span<const uint8_t> spake2_key);
// MAC_a or MAC_b, by who sends it.
std::array<uint8_t, 32> PairingConfirmMac(const PairingKeys& keys,
                                          PairingRole sender);

// SPAKE2 names: "compass-a:" + host + ":" + slot and "compass-b:...".
std::string PairingSpakeName(PairingRole role, std::string_view host, int slot);

struct PairingMessage {
  int seq = 0;
  std::vector<uint8_t> data;
};

// The server's pairing endpoints. Every callback gets the HTTP status of
// the reply, or 0 if there was none or it couldn't be parsed.
class PairingTransport {
 public:
  using OpenCallback = base::OnceCallback<
      void(int status, int slot, base::TimeDelta expires_in)>;
  using StatusCallback = base::OnceCallback<void(int status)>;
  using ReceiveCallback =
      base::OnceCallback<void(int status, std::vector<PairingMessage>)>;

  virtual ~PairingTransport() = default;

  // A: POST /compass/pair (signed).
  virtual void Open(OpenCallback callback) = 0;
  // B: POST /compass/pair/{slot}/join. Keeps the b_token for B's later
  // requests.
  virtual void Join(int slot, StatusCallback callback) = 0;
  // POST /compass/pair/{slot}/msg.
  virtual void Send(int slot,
                    PairingRole from,
                    std::vector<uint8_t> data,
                    StatusCallback callback) = 0;
  // GET /compass/pair/{slot}/msg?for=<reader>&after=<after>, long poll.
  virtual void Receive(int slot,
                       PairingRole reader,
                       int after,
                       ReceiveCallback callback) = 0;
  // DELETE /compass/pair/{slot}. Fire and forget: it may outlive |this|.
  virtual void Close(int slot, PairingRole by) = 0;
};

// A's side.
class PairingHost {
 public:
  enum class Result {
    kAdded,
    kDenied,
    // B's confirmation didn't match: a wrong code, or someone in the middle.
    kWrongCode,
    kExpired,
    kRateLimited,
    kFailed,
  };

  class Delegate {
   public:
    virtual ~Delegate() = default;
    // The code to show, and how long it's valid.
    virtual void OnPairingCode(const std::string& code,
                               base::TimeDelta expires_in) = 0;
    // B proved it knows the code; Answer() lets it in or not.
    virtual void OnPairingRequest(const std::string& device_name) = 0;
    // Last call; the delegate may delete |this| from it.
    virtual void OnPairingFinished(Result result) = 0;
  };

  // |host| is the server's host name, bound into SPAKE2.
  PairingHost(std::unique_ptr<PairingTransport> transport,
              std::string host,
              const Seed& seed,
              Delegate* delegate);
  PairingHost(const PairingHost&) = delete;
  PairingHost& operator=(const PairingHost&) = delete;
  // Closes the slot unless the seed was already sent.
  ~PairingHost();

  void Start();
  // After OnPairingRequest().
  void Answer(bool allow);

 private:
  enum class Step {
    kIdle,
    kOpening,
    kWaitingForB,
    kWaitingForConfirmation,
    kAsking,
    kAnswering,
    kDone,
  };

  void OnOpened(int status, int slot, base::TimeDelta expires_in);
  void Poll();
  void OnReceived(int status, std::vector<PairingMessage> messages);
  // Returns false if pairing ended.
  bool HandleMessage(const PairingMessage& message);
  void OnAlicePosted(int status);
  void OnAnswerPosted(bool allow, int status);
  void OnExpired();
  // Ends with |result|, closing the slot if |close|.
  void Finish(Result result, bool close);

  std::unique_ptr<PairingTransport> transport_;
  const std::string host_;
  const Seed seed_;
  raw_ptr<Delegate> delegate_;

  Step step_ = Step::kIdle;
  int slot_ = 0;
  int last_seq_ = 0;
  bool slot_open_ = false;
  bssl::UniquePtr<SPAKE2_CTX> spake_;
  std::vector<uint8_t> alice_message_;
  std::optional<PairingKeys> keys_;
  base::OneShotTimer expiry_timer_;
  base::OneShotTimer retry_timer_;

  base::WeakPtrFactory<PairingHost> weak_factory_{this};
};

// B's side.
class PairingJoiner {
 public:
  enum class Result {
    kJoined,
    kDenied,
    // Not "7-482913".
    kBadCode,
    // No open slot with that number (or it expired): 404 on join.
    kNoSuchCode,
    // Someone already joined that slot: 409.
    kCodeUsed,
    // A's confirmation didn't match.
    kWrongCode,
    // A closed the slot while B waited: A saw a wrong code, or was
    // canceled, or the slot expired.
    kClosed,
    kExpired,
    kRateLimited,
    kFailed,
  };
  using DoneCallback =
      base::OnceCallback<void(Result result, std::optional<Seed> seed)>;

  // How long B waits for the whole exchange, the user's answer on A
  // included. The slot itself lives 5 minutes.
  static constexpr base::TimeDelta kTimeout =
      base::Minutes(5) + base::Seconds(30);

  PairingJoiner(std::unique_ptr<PairingTransport> transport,
                std::string host,
                std::string device_name);
  PairingJoiner(const PairingJoiner&) = delete;
  PairingJoiner& operator=(const PairingJoiner&) = delete;
  // Closes the slot if pairing is still going.
  ~PairingJoiner();

  // |waiting| runs once B has proved the code and waits for A's answer.
  // |done| runs last; the caller may delete |this| from it.
  void Start(std::string_view code,
             base::OnceClosure waiting,
             DoneCallback done);

 private:
  enum class Step {
    kIdle,
    kJoining,
    kSendingBob,
    kWaitingForAlice,
    kSendingConfirmation,
    kWaitingForAnswer,
    kDone,
  };

  void OnJoined(int status);
  void OnBobPosted(int status);
  void Poll();
  void OnReceived(int status, std::vector<PairingMessage> messages);
  // Returns false if pairing ended.
  bool HandleMessage(const PairingMessage& message);
  void OnConfirmationPosted(int status);
  void OnTimeout();
  void Finish(Result result, std::optional<Seed> seed, bool close);

  std::unique_ptr<PairingTransport> transport_;
  const std::string host_;
  const std::string device_name_;
  base::OnceClosure waiting_;
  DoneCallback done_;

  Step step_ = Step::kIdle;
  PairingCode code_;
  int last_seq_ = 0;
  bool joined_ = false;
  bssl::UniquePtr<SPAKE2_CTX> spake_;
  std::optional<PairingKeys> keys_;
  base::OneShotTimer timeout_timer_;
  base::OneShotTimer retry_timer_;

  base::WeakPtrFactory<PairingJoiner> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_PAIRING_H_
