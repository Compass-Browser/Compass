// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/pairing.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::sync {
namespace {

constexpr char kHost[] = "sync.example.com";
constexpr char kDeviceName[] = "MacBook";
constexpr base::TimeDelta kSlotLifetime = base::Seconds(300);
constexpr base::TimeDelta kLongPoll = base::Seconds(25);

// The pairing endpoints of docs/design/sync.md, "Pairing API", in memory.
class FakePairingServer {
 public:
  struct Slot {
    base::TimeTicks expires;
    bool joined = false;
    // Messages by sender.
    std::vector<PairingMessage> from_a;
    std::vector<PairingMessage> from_b;
    // A pending long poll per reader.
    struct Poll {
      int after = 0;
      PairingTransport::ReceiveCallback callback;
    };
    std::optional<Poll> poll_a;
    std::optional<Poll> poll_b;
    int closes = 0;
  };

  // Overrides the next join's status (409/429 tests).
  void set_join_status(int status) { join_status_ = status; }
  void set_open_status(int status) { open_status_ = status; }

  void Open(PairingTransport::OpenCallback callback) {
    if (open_status_) {
      Reply(base::BindOnce(std::move(callback), *open_status_, 0,
                           base::TimeDelta()));
      return;
    }
    // One open slot per chain.
    if (open_slot_) {
      Delete(*open_slot_);
    }
    int number = 1;
    while (slots_.contains(number)) {
      ++number;
    }
    slots_[number].expires = base::TimeTicks::Now() + kSlotLifetime;
    open_slot_ = number;
    Reply(base::BindOnce(std::move(callback), 201, number, kSlotLifetime));
  }

  void Join(int number, PairingTransport::StatusCallback callback) {
    if (join_status_) {
      Reply(base::BindOnce(std::move(callback), *join_status_));
      return;
    }
    Slot* slot = Find(number);
    if (!slot) {
      Reply(base::BindOnce(std::move(callback), 404));
      return;
    }
    if (slot->joined) {
      Reply(base::BindOnce(std::move(callback), 409));
      return;
    }
    slot->joined = true;
    Reply(base::BindOnce(std::move(callback), 200));
  }

  void Send(int number,
            PairingRole from,
            std::vector<uint8_t> data,
            PairingTransport::StatusCallback callback) {
    Slot* slot = Find(number);
    if (!slot) {
      Reply(base::BindOnce(std::move(callback), 404));
      return;
    }
    std::vector<PairingMessage>& messages =
        from == PairingRole::kA ? slot->from_a : slot->from_b;
    if (messages.size() >= 8) {
      Reply(base::BindOnce(std::move(callback), 429));
      return;
    }
    messages.push_back(
        {static_cast<int>(messages.size()) + 1, std::move(data)});
    Reply(base::BindOnce(std::move(callback), 200));
    // Wake the other side's long poll.
    std::optional<Slot::Poll>& poll =
        from == PairingRole::kA ? slot->poll_b : slot->poll_a;
    if (poll) {
      Slot::Poll pending = std::move(*poll);
      poll.reset();
      Answer(*slot, from, pending.after, std::move(pending.callback));
    }
  }

  void Receive(int number,
               PairingRole reader,
               int after,
               PairingTransport::ReceiveCallback callback) {
    Slot* slot = Find(number);
    if (!slot) {
      Reply(base::BindOnce(std::move(callback), 404,
                           std::vector<PairingMessage>()));
      return;
    }
    const PairingRole sender =
        reader == PairingRole::kA ? PairingRole::kB : PairingRole::kA;
    const std::vector<PairingMessage>& messages =
        sender == PairingRole::kA ? slot->from_a : slot->from_b;
    if (static_cast<int>(messages.size()) > after) {
      Answer(*slot, sender, after, std::move(callback));
      return;
    }
    std::optional<Slot::Poll>& poll =
        reader == PairingRole::kA ? slot->poll_a : slot->poll_b;
    poll = Slot::Poll{after, std::move(callback)};
    // An empty answer when the long poll times out.
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&FakePairingServer::TimeOutPoll,
                       weak_factory_.GetWeakPtr(), number, reader),
        kLongPoll);
  }

  void Close(int number) {
    if (Find(number)) {
      ++closes_;
      Delete(number);
    }
  }

  // Everything A sent, for leak checks.
  std::vector<std::vector<uint8_t>> SentByA(int number) const {
    auto it = sent_by_a_.find(number);
    return it == sent_by_a_.end() ? std::vector<std::vector<uint8_t>>()
                                  : it->second;
  }
  int closes() const { return closes_; }
  bool HasSlot(int number) { return Find(number) != nullptr; }

  void RecordSentByA(int number, const std::vector<uint8_t>& data) {
    sent_by_a_[number].push_back(data);
  }

 private:
  // Replies asynchronously, as the network does.
  void Reply(base::OnceClosure reply) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                             std::move(reply));
  }

  void Answer(const Slot& slot,
              PairingRole sender,
              int after,
              PairingTransport::ReceiveCallback callback) {
    const std::vector<PairingMessage>& messages =
        sender == PairingRole::kA ? slot.from_a : slot.from_b;
    std::vector<PairingMessage> newer;
    for (const PairingMessage& message : messages) {
      if (message.seq > after) {
        newer.push_back(message);
      }
    }
    Reply(base::BindOnce(std::move(callback), 200, std::move(newer)));
  }

  void TimeOutPoll(int number, PairingRole reader) {
    Slot* slot = Find(number);
    if (!slot) {
      return;
    }
    std::optional<Slot::Poll>& poll =
        reader == PairingRole::kA ? slot->poll_a : slot->poll_b;
    if (poll) {
      PairingTransport::ReceiveCallback callback = std::move(poll->callback);
      poll.reset();
      Reply(base::BindOnce(std::move(callback), 200,
                           std::vector<PairingMessage>()));
    }
  }

  // The slot, or null if it's gone or expired (then it's deleted).
  Slot* Find(int number) {
    auto it = slots_.find(number);
    if (it == slots_.end()) {
      return nullptr;
    }
    if (base::TimeTicks::Now() >= it->second.expires) {
      Delete(number);
      return nullptr;
    }
    return &it->second;
  }

  // Deletes a slot; pending polls get 404.
  void Delete(int number) {
    auto it = slots_.find(number);
    if (it == slots_.end()) {
      return;
    }
    Slot slot = std::move(it->second);
    slots_.erase(it);
    if (open_slot_ == number) {
      open_slot_.reset();
    }
    for (std::optional<Slot::Poll>* poll : {&slot.poll_a, &slot.poll_b}) {
      if (*poll) {
        Reply(base::BindOnce(std::move((*poll)->callback), 404,
                             std::vector<PairingMessage>()));
      }
    }
  }

  std::map<int, Slot> slots_;
  std::optional<int> open_slot_;
  std::optional<int> join_status_;
  std::optional<int> open_status_;
  std::map<int, std::vector<std::vector<uint8_t>>> sent_by_a_;
  int closes_ = 0;
  base::WeakPtrFactory<FakePairingServer> weak_factory_{this};
};

// One device's connection to the fake server.
class FakeTransport : public PairingTransport {
 public:
  explicit FakeTransport(base::WeakPtr<FakePairingServer> server)
      : server_(server) {}

  void Open(OpenCallback callback) override {
    server_->Open(std::move(callback));
  }
  void Join(int slot, StatusCallback callback) override {
    server_->Join(slot, std::move(callback));
  }
  void Send(int slot,
            PairingRole from,
            std::vector<uint8_t> data,
            StatusCallback callback) override {
    if (from == PairingRole::kA) {
      server_->RecordSentByA(slot, data);
    }
    server_->Send(slot, from, std::move(data), std::move(callback));
  }
  void Receive(int slot,
               PairingRole reader,
               int after,
               ReceiveCallback callback) override {
    server_->Receive(slot, reader, after, std::move(callback));
  }
  void Close(int slot, PairingRole by) override {
    if (server_) {
      server_->Close(slot);
    }
  }

 private:
  base::WeakPtr<FakePairingServer> server_;
};

// Records what A's host reports, and answers when asked if told to.
class HostDelegate : public PairingHost::Delegate {
 public:
  void OnPairingCode(const std::string& code,
                     base::TimeDelta expires_in) override {
    code_ = code;
    expires_in_ = expires_in;
  }
  void OnPairingRequest(const std::string& device_name) override {
    requests_.push_back(device_name);
  }
  void OnPairingFinished(PairingHost::Result result) override {
    result_ = result;
  }

  const std::string& code() const { return code_; }
  base::TimeDelta expires_in() const { return expires_in_; }
  const std::vector<std::string>& requests() const { return requests_; }
  std::optional<PairingHost::Result> result() const { return result_; }

 private:
  std::string code_;
  base::TimeDelta expires_in_;
  std::vector<std::string> requests_;
  std::optional<PairingHost::Result> result_;
};

struct JoinOutcome {
  std::optional<PairingJoiner::Result> result;
  std::optional<Seed> seed;
  bool waited_for_answer = false;
};

class PairingTest : public testing::Test {
 protected:
  PairingTest() {
    for (size_t i = 0; i < seed_.size(); ++i) {
      seed_[i] = static_cast<uint8_t>(0xa0 + i);
    }
  }

  std::unique_ptr<PairingHost> StartHost() {
    auto host = std::make_unique<PairingHost>(
        std::make_unique<FakeTransport>(server_.GetWeakPtr()), kHost, seed_,
        &host_delegate_);
    host->Start();
    task_environment_.RunUntilIdle();
    return host;
  }

  std::unique_ptr<PairingJoiner> StartJoiner(const std::string& code,
                                             JoinOutcome* outcome) {
    auto joiner = std::make_unique<PairingJoiner>(
        std::make_unique<FakeTransport>(server_.GetWeakPtr()), kHost,
        kDeviceName);
    joiner->Start(
        code,
        base::BindOnce(
            [](JoinOutcome* outcome) { outcome->waited_for_answer = true; },
            outcome),
        base::BindOnce(
            [](JoinOutcome* outcome, PairingJoiner::Result result,
               std::optional<Seed> seed) {
              outcome->result = result;
              outcome->seed = seed;
            },
            outcome));
    task_environment_.RunUntilIdle();
    return joiner;
  }

  // The code with its digits changed.
  static std::string WrongDigits(const std::string& code) {
    std::optional<PairingCode> parsed = ParsePairingCode(code);
    EXPECT_TRUE(parsed);
    std::string& digits = parsed->digits;
    digits[0] = digits[0] == '9' ? '0' : digits[0] + 1;
    return FormatPairingCode(*parsed);
  }

  bool SeedLeaked(int slot) const {
    for (const std::vector<uint8_t>& message : server_.SentByA(slot)) {
      if (std::search(message.begin(), message.end(), seed_.begin(),
                      seed_.end()) != message.end()) {
        return true;
      }
    }
    return false;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakePairingServer server_;
  HostDelegate host_delegate_;
  Seed seed_;
};

TEST(PairingCodeTest, ParseAndFormat) {
  std::optional<PairingCode> code = ParsePairingCode("7-482913");
  ASSERT_TRUE(code);
  EXPECT_EQ(code->slot, 7);
  EXPECT_EQ(code->digits, "482913");
  EXPECT_EQ(FormatPairingCode(*code), "7-482913");

  code = ParsePairingCode(" 12 - 004213 ");
  ASSERT_TRUE(code);
  EXPECT_EQ(code->slot, 12);
  EXPECT_EQ(code->digits, "004213");

  EXPECT_FALSE(ParsePairingCode(""));
  EXPECT_FALSE(ParsePairingCode("7"));
  EXPECT_FALSE(ParsePairingCode("7-48291"));
  EXPECT_FALSE(ParsePairingCode("7-4829134"));
  EXPECT_FALSE(ParsePairingCode("0-482913"));
  EXPECT_FALSE(ParsePairingCode("-482913"));
  EXPECT_FALSE(ParsePairingCode("a-482913"));
  EXPECT_FALSE(ParsePairingCode("7-48291x"));
}

// Fixed in docs/design/sync.md, "Pairing API": K = 00 01 .. 3f.
TEST(PairingKeysTest, Vectors) {
  std::array<uint8_t, 64> k;
  for (size_t i = 0; i < k.size(); ++i) {
    k[i] = static_cast<uint8_t>(i);
  }
  const PairingKeys keys = DerivePairingKeys(k);
  EXPECT_EQ(base::HexEncodeLower(keys.enc),
            "5144144d90ed02f3805d0a59510e4a0c7d7454adf0f4cda9e20390497a857653");
  EXPECT_EQ(base::HexEncodeLower(keys.confirm_a),
            "d71f8461386ed03eb405b0d5fa82478a93b636c943b35260c2e9af7863ef6a2a");
  EXPECT_EQ(base::HexEncodeLower(keys.confirm_b),
            "46d4fef69618607a51b39229754d45363e452737e49b53c98009c6b7dd61a05a");
  EXPECT_EQ(base::HexEncodeLower(PairingConfirmMac(keys, PairingRole::kA)),
            "5640ef8734b1be15e7d60a12bac07ba7583b237137037eafc44bff2d7f3366f8");
  EXPECT_EQ(base::HexEncodeLower(PairingConfirmMac(keys, PairingRole::kB)),
            "d0ccb754ea5fd62bed000484f428a444a674852d9725b1e65c7aa62a12ff0ac4");
  EXPECT_EQ(PairingSpakeName(PairingRole::kA, "sync.example.com", 7),
            "compass-a:sync.example.com:7");
  EXPECT_EQ(PairingSpakeName(PairingRole::kB, "sync.example.com", 7),
            "compass-b:sync.example.com:7");
}

TEST_F(PairingTest, RightCodeAddsTheDevice) {
  std::unique_ptr<PairingHost> host = StartHost();
  ASSERT_FALSE(host_delegate_.code().empty());
  EXPECT_EQ(host_delegate_.expires_in(), kSlotLifetime);
  EXPECT_EQ(ParsePairingCode(host_delegate_.code())->slot, 1);

  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &outcome);
  ASSERT_EQ(host_delegate_.requests(), std::vector<std::string>{kDeviceName});
  EXPECT_TRUE(outcome.waited_for_answer);
  EXPECT_FALSE(outcome.result);
  EXPECT_FALSE(SeedLeaked(1));

  host->Answer(true);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(host_delegate_.result(), PairingHost::Result::kAdded);
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kJoined);
  EXPECT_EQ(outcome.seed, seed_);
  // The seed only travels encrypted.
  EXPECT_FALSE(SeedLeaked(1));
  // B closed the slot once it had the answer.
  EXPECT_FALSE(server_.HasSlot(1));
}

TEST_F(PairingTest, WrongCodeFailsOnBothSidesAndLeaksNothing) {
  std::unique_ptr<PairingHost> host = StartHost();
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(WrongDigits(host_delegate_.code()), &outcome);

  EXPECT_EQ(host_delegate_.result(), PairingHost::Result::kWrongCode);
  EXPECT_TRUE(host_delegate_.requests().empty());
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kClosed);
  EXPECT_FALSE(outcome.seed);
  // A sent its SPAKE2 message only: no answer, no seed.
  EXPECT_EQ(server_.SentByA(1).size(), 1u);
  EXPECT_FALSE(SeedLeaked(1));
  // A burned the code.
  EXPECT_FALSE(server_.HasSlot(1));

  // A new code works.
  std::unique_ptr<PairingHost> second_host;
  HostDelegate second_delegate;
  second_host = std::make_unique<PairingHost>(
      std::make_unique<FakeTransport>(server_.GetWeakPtr()), kHost, seed_,
      &second_delegate);
  second_host->Start();
  task_environment_.RunUntilIdle();
  JoinOutcome second;
  std::unique_ptr<PairingJoiner> second_joiner =
      StartJoiner(second_delegate.code(), &second);
  second_host->Answer(true);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(second.result, PairingJoiner::Result::kJoined);
  EXPECT_EQ(second.seed, seed_);
}

TEST_F(PairingTest, Deny) {
  std::unique_ptr<PairingHost> host = StartHost();
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &outcome);
  host->Answer(false);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(host_delegate_.result(), PairingHost::Result::kDenied);
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kDenied);
  EXPECT_FALSE(outcome.seed);
  EXPECT_FALSE(SeedLeaked(1));
}

TEST_F(PairingTest, AnswerBeforeRequestIsIgnored) {
  std::unique_ptr<PairingHost> host = StartHost();
  host->Answer(true);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(host_delegate_.result());
  EXPECT_TRUE(server_.SentByA(1).empty());
}

TEST_F(PairingTest, CodeExpires) {
  std::unique_ptr<PairingHost> host = StartHost();
  const std::string code = host_delegate_.code();
  task_environment_.FastForwardBy(kSlotLifetime + base::Seconds(1));
  EXPECT_EQ(host_delegate_.result(), PairingHost::Result::kExpired);

  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner = StartJoiner(code, &outcome);
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kNoSuchCode);
}

TEST_F(PairingTest, ExpiresWhileWaitingForTheAnswer) {
  std::unique_ptr<PairingHost> host = StartHost();
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &outcome);
  ASSERT_EQ(host_delegate_.requests().size(), 1u);
  // Nobody answers on A.
  task_environment_.FastForwardBy(PairingJoiner::kTimeout);
  EXPECT_EQ(host_delegate_.result(), PairingHost::Result::kExpired);
  ASSERT_TRUE(outcome.result);
  EXPECT_TRUE(*outcome.result == PairingJoiner::Result::kClosed ||
              *outcome.result == PairingJoiner::Result::kExpired);
  EXPECT_FALSE(outcome.seed);
  // Answering late does nothing.
  host->Answer(true);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(SeedLeaked(1));
}

TEST_F(PairingTest, CancelOnAClosesTheSlot) {
  std::unique_ptr<PairingHost> host = StartHost();
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &outcome);
  host.reset();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kClosed);
  EXPECT_FALSE(server_.HasSlot(1));
}

TEST_F(PairingTest, CancelOnBClosesTheSlot) {
  std::unique_ptr<PairingHost> host = StartHost();
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &outcome);
  joiner.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(server_.HasSlot(1));
  // A learns when it answers.
  host->Answer(true);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(host_delegate_.result(), PairingHost::Result::kExpired);
  EXPECT_FALSE(SeedLeaked(1));
}

TEST_F(PairingTest, UnknownSlot404) {
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner = StartJoiner("9-123456", &outcome);
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kNoSuchCode);
}

TEST_F(PairingTest, SecondJoin409) {
  std::unique_ptr<PairingHost> host = StartHost();
  JoinOutcome first;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &first);
  JoinOutcome second;
  std::unique_ptr<PairingJoiner> other =
      StartJoiner(host_delegate_.code(), &second);
  EXPECT_EQ(second.result, PairingJoiner::Result::kCodeUsed);
  // The first one goes on.
  host->Answer(true);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(first.result, PairingJoiner::Result::kJoined);
}

TEST_F(PairingTest, RateLimited429) {
  std::unique_ptr<PairingHost> host = StartHost();
  server_.set_join_status(429);
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &outcome);
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kRateLimited);
  EXPECT_FALSE(host_delegate_.result());
}

TEST_F(PairingTest, OpenFails) {
  server_.set_open_status(429);
  std::unique_ptr<PairingHost> host = StartHost();
  EXPECT_EQ(host_delegate_.result(), PairingHost::Result::kRateLimited);
  EXPECT_TRUE(host_delegate_.code().empty());
}

TEST_F(PairingTest, BadCode) {
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner = StartJoiner("482913", &outcome);
  EXPECT_EQ(outcome.result, PairingJoiner::Result::kBadCode);
}

TEST_F(PairingTest, LongPollsRepeat) {
  std::unique_ptr<PairingHost> host = StartHost();
  // B shows up after several empty long polls.
  task_environment_.FastForwardBy(kLongPoll * 3 + base::Seconds(1));
  EXPECT_FALSE(host_delegate_.result());
  JoinOutcome outcome;
  std::unique_ptr<PairingJoiner> joiner =
      StartJoiner(host_delegate_.code(), &outcome);
  EXPECT_EQ(host_delegate_.requests().size(), 1u);
}

}  // namespace
}  // namespace compass::sync
