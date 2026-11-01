// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sync/compass_sync_page_handler.h"

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/observer_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

using ::testing::ElementsAre;

class FakeController : public sync::SyncController {
 public:
  void AddObserver(Observer* observer) override {
    observers_.AddObserver(observer);
  }
  void RemoveObserver(Observer* observer) override {
    observers_.RemoveObserver(observer);
  }
  sync::SyncStatus GetStatus() const override { return status_; }
  sync::PairingStatus GetPairingStatus() const override { return pairing_; }
  void StartNewChain(const std::string& server_url,
                     StartCallback callback) override {
    calls_.push_back("start " + server_url);
    if (server_url.empty()) {
      std::move(callback).Run(false, "no server", {});
      return;
    }
    std::move(callback).Run(true, std::string(), {"abandon", "about"});
  }
  void JoinWithRecoveryCode(const std::string& server_url,
                            const std::string& words,
                            DoneCallback callback) override {
    calls_.push_back("recover " + server_url + " " + words);
    std::move(callback).Run(false, "bad words");
  }
  void JoinWithPairingCode(const std::string& server_url,
                           const std::string& code) override {
    calls_.push_back("join " + server_url + " " + code);
  }
  void AddDevice() override { calls_.push_back("add"); }
  void AnswerPairing(bool allow) override {
    calls_.push_back(allow ? "allow" : "deny");
  }
  void CancelPairing() override { calls_.push_back("cancel"); }
  void Leave() override { calls_.push_back("leave"); }
  void Restart() override { calls_.push_back("restart"); }
  std::vector<sync::SyncDevice> GetDevices() const override { return devices_; }
  std::vector<sync::SyncDataTypeState> GetDataTypes() const override {
    return types_;
  }
  void SetDataTypeEnabled(sync::SyncDataType type, bool enabled) override {
    calls_.push_back(std::string("type ") +
                     std::to_string(static_cast<int>(type)) +
                     (enabled ? " on" : " off"));
  }
  std::vector<std::string> GetRecoveryCode() const override { return words_; }
  void ResetChain(StartCallback callback) override {
    calls_.push_back("reset");
    std::move(callback).Run(true, std::string(), {"zoo", "wrong"});
  }

  void SetDetails(std::vector<sync::SyncDevice> devices,
                  std::vector<sync::SyncDataTypeState> types) {
    devices_ = std::move(devices);
    types_ = std::move(types);
  }
  void set_words(std::vector<std::string> words) { words_ = std::move(words); }

  void Set(sync::SyncStatus status, sync::PairingStatus pairing) {
    status_ = std::move(status);
    pairing_ = std::move(pairing);
    for (Observer& observer : observers_) {
      observer.OnSyncControllerChanged();
    }
  }
  bool HasObservers() const { return !observers_.empty(); }
  const std::vector<std::string>& calls() const { return calls_; }

 private:
  sync::SyncStatus status_;
  sync::PairingStatus pairing_;
  std::vector<sync::SyncDevice> devices_;
  std::vector<sync::SyncDataTypeState> types_;
  std::vector<std::string> words_;
  std::vector<std::string> calls_;
  base::ObserverList<Observer> observers_;
};

class FakePage : public sync::mojom::Page {
 public:
  mojo::PendingRemote<sync::mojom::Page> BindAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void OnStateChanged(sync::mojom::StatusPtr status,
                      sync::mojom::PairingPtr pairing,
                      sync::mojom::DetailsPtr details) override {
    status_ = std::move(status);
    pairing_ = std::move(pairing);
    details_ = std::move(details);
  }
  const sync::mojom::StatusPtr& status() const { return status_; }
  const sync::mojom::PairingPtr& pairing() const { return pairing_; }
  const sync::mojom::DetailsPtr& details() const { return details_; }

 private:
  sync::mojom::StatusPtr status_;
  sync::mojom::PairingPtr pairing_;
  sync::mojom::DetailsPtr details_;
  mojo::Receiver<sync::mojom::Page> receiver_{this};
};

class CompassSyncPageHandlerTest : public testing::Test {
 protected:
  CompassSyncPageHandlerTest()
      : handler_(std::make_unique<CompassSyncPageHandler>(
            remote_.BindNewPipeAndPassReceiver(),
            page_.BindAndPassRemote(),
            &controller_,
            base::BindRepeating(&CompassSyncPageHandlerTest::Reauthenticate,
                                base::Unretained(this)))) {}

  // The fake Touch ID: answers as |reauth_answer_| says, counting the asks.
  void Reauthenticate(base::OnceCallback<void(bool)> done) {
    ++reauth_asks_;
    std::move(done).Run(reauth_answer_);
  }

  base::test::TaskEnvironment task_environment_;
  bool reauth_answer_ = true;
  int reauth_asks_ = 0;
  FakeController controller_;
  FakePage page_;
  mojo::Remote<sync::mojom::PageHandler> remote_;
  std::unique_ptr<CompassSyncPageHandler> handler_;
};

TEST_F(CompassSyncPageHandlerTest, GetState) {
  controller_.Set({sync::SyncState::kOn, "http://h:8295/v2", "Last synced"},
                  {sync::PairingStep::kShowingCode, "7-482913", "", ""});
  base::test::TestFuture<sync::mojom::StatusPtr, sync::mojom::PairingPtr,
                         sync::mojom::DetailsPtr>
      future;
  remote_->GetState(future.GetCallback());
  auto [status, pairing, details] = future.Take();
  ASSERT_TRUE(details);
  EXPECT_TRUE(details->devices.empty());
  EXPECT_EQ(status->state, sync::mojom::State::kOn);
  EXPECT_EQ(status->server_url, "http://h:8295/v2");
  EXPECT_EQ(status->message, "Last synced");
  EXPECT_EQ(status->menu_label, "Synced");
  EXPECT_EQ(pairing->step, sync::mojom::PairingStep::kShowingCode);
  EXPECT_EQ(pairing->code, "7-482913");
}

TEST_F(CompassSyncPageHandlerTest, StartSyncReturnsTheWords) {
  base::test::TestFuture<bool, const std::string&,
                         const std::vector<std::string>&>
      future;
  remote_->StartSync("http://h:8295/v2", future.GetCallback());
  EXPECT_TRUE(future.Get<0>());
  EXPECT_THAT(future.Get<2>(), ElementsAre("abandon", "about"));

  base::test::TestFuture<bool, const std::string&,
                         const std::vector<std::string>&>
      failed;
  remote_->StartSync("", failed.GetCallback());
  EXPECT_FALSE(failed.Get<0>());
  EXPECT_EQ(failed.Get<1>(), "no server");
  EXPECT_TRUE(failed.Get<2>().empty());
}

TEST_F(CompassSyncPageHandlerTest, JoinWithRecoveryCode) {
  base::test::TestFuture<bool, const std::string&> future;
  remote_->JoinWithRecoveryCode("http://h/v2", "zoo zoo", future.GetCallback());
  EXPECT_FALSE(future.Get<0>());
  EXPECT_EQ(future.Get<1>(), "bad words");
  EXPECT_THAT(controller_.calls(), ElementsAre("recover http://h/v2 zoo zoo"));
}

TEST_F(CompassSyncPageHandlerTest, ForwardsActions) {
  remote_->JoinWithPairingCode("http://h/v2", "7-482913");
  remote_->AddDevice();
  remote_->AnswerPairing(true);
  remote_->AnswerPairing(false);
  remote_->CancelPairing();
  remote_->Leave();
  remote_->Restart();
  remote_.FlushForTesting();
  EXPECT_THAT(controller_.calls(),
              ElementsAre("join http://h/v2 7-482913", "add", "allow", "deny",
                          "cancel", "leave", "restart"));
}

TEST_F(CompassSyncPageHandlerTest, PushesChanges) {
  controller_.Set({sync::SyncState::kStarting, "http://h/v2", "Starting…"},
                  {sync::PairingStep::kConfirm, "7-482913", "MacBook", ""});
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(page_.status());
  EXPECT_EQ(page_.status()->state, sync::mojom::State::kStarting);
  ASSERT_TRUE(page_.pairing());
  EXPECT_EQ(page_.pairing()->step, sync::mojom::PairingStep::kConfirm);
  EXPECT_EQ(page_.pairing()->device_name, "MacBook");
  ASSERT_TRUE(page_.details());
}

TEST_F(CompassSyncPageHandlerTest, DevicesAndTypes) {
  sync::SyncDevice mac;
  mac.name = "Studio";
  mac.kind = "Mac";
  mac.last_seen =
      base::Time::FromMillisecondsSinceUnixEpoch(int64_t{1'800'000'000'000});
  mac.this_device = true;
  sync::SyncDevice phone;
  phone.name = "Phone";
  phone.kind = "iPhone";
  controller_.SetDetails({mac, phone}, {{sync::SyncDataType::kPasswords, true},
                                        {sync::SyncDataType::kBookmarks, false},
                                        {sync::SyncDataType::kSpaces, true}});
  base::test::TestFuture<sync::mojom::StatusPtr, sync::mojom::PairingPtr,
                         sync::mojom::DetailsPtr>
      future;
  remote_->GetState(future.GetCallback());
  const sync::mojom::DetailsPtr details = std::get<2>(future.Take());
  ASSERT_EQ(details->devices.size(), 2u);
  EXPECT_EQ(details->devices[0]->name, "Studio");
  EXPECT_EQ(details->devices[0]->kind, "Mac");
  EXPECT_EQ(details->devices[0]->last_seen_ms, 1'800'000'000'000.0);
  EXPECT_TRUE(details->devices[0]->this_device);
  EXPECT_EQ(details->devices[1]->last_seen_ms, 0.0);
  EXPECT_FALSE(details->devices[1]->this_device);
  ASSERT_EQ(details->types.size(), 3u);
  EXPECT_EQ(details->types[1]->type, sync::mojom::DataType::kBookmarks);
  EXPECT_FALSE(details->types[1]->enabled);
  EXPECT_EQ(details->types[2]->type, sync::mojom::DataType::kSpaces);
  EXPECT_TRUE(details->types[2]->enabled);
}

TEST_F(CompassSyncPageHandlerTest, SwitchesReachTheController) {
  remote_->SetDataTypeEnabled(sync::mojom::DataType::kBookmarks, false);
  remote_->SetDataTypeEnabled(sync::mojom::DataType::kSpaces, true);
  remote_.FlushForTesting();
  EXPECT_THAT(
      controller_.calls(),
      ElementsAre(
          "type " +
              std::to_string(static_cast<int>(sync::SyncDataType::kBookmarks)) +
              " off",
          "type " +
              std::to_string(static_cast<int>(sync::SyncDataType::kSpaces)) +
              " on"));
}

TEST_F(CompassSyncPageHandlerTest, RecoveryCodeNeedsReauth) {
  controller_.set_words({"abandon", "amount"});

  reauth_answer_ = false;
  base::test::TestFuture<bool, const std::string&,
                         const std::vector<std::string>&>
      refused;
  remote_->RevealRecoveryCode(refused.GetCallback());
  EXPECT_FALSE(refused.Get<0>());
  EXPECT_TRUE(refused.Get<2>().empty());
  EXPECT_EQ(reauth_asks_, 1);

  reauth_answer_ = true;
  base::test::TestFuture<bool, const std::string&,
                         const std::vector<std::string>&>
      shown;
  remote_->RevealRecoveryCode(shown.GetCallback());
  EXPECT_TRUE(shown.Get<0>());
  EXPECT_THAT(shown.Get<2>(), ElementsAre("abandon", "amount"));
  EXPECT_EQ(reauth_asks_, 2);
}

TEST_F(CompassSyncPageHandlerTest, NoRecoveryCodeWithoutAChain) {
  base::test::TestFuture<bool, const std::string&,
                         const std::vector<std::string>&>
      future;
  remote_->RevealRecoveryCode(future.GetCallback());
  EXPECT_FALSE(future.Get<0>());
  EXPECT_FALSE(future.Get<1>().empty());
  // Nothing to protect: no prompt.
  EXPECT_EQ(reauth_asks_, 0);
}

TEST_F(CompassSyncPageHandlerTest, ResetSyncReturnsTheNewWords) {
  base::test::TestFuture<bool, const std::string&,
                         const std::vector<std::string>&>
      future;
  remote_->ResetSync(future.GetCallback());
  EXPECT_TRUE(future.Get<0>());
  EXPECT_THAT(future.Get<2>(), ElementsAre("zoo", "wrong"));
  EXPECT_THAT(controller_.calls(), ElementsAre("reset"));
}

TEST_F(CompassSyncPageHandlerTest, StopsObservingWhenGone) {
  EXPECT_TRUE(controller_.HasObservers());
  handler_.reset();
  EXPECT_FALSE(controller_.HasObservers());
}

// Slice 5: the mapping step goes through to the controller.
class MappingController : public FakeController {
 public:
  sync::ProfileMappingStatus GetProfileMapping() const override {
    sync::ProfileMappingStatus status;
    status.needed = true;
    status.chain = {{"u-home", "Personal", 3, true},
                    {"u-work", "Work", -1, false}};
    status.local = {{"Profile 1", "u-local", "Work", 0}};
    status.proposed = {{"Profile 1", "u-work"}};
    return status;
  }
  void SetProfileMapping(const sync::ProfileMapping& mapping,
                         DoneCallback callback) override {
    mapping_ = mapping;
    std::move(callback).Run(true, std::string());
  }
  const sync::ProfileMapping& mapping() const { return mapping_; }

 private:
  sync::ProfileMapping mapping_;
};

TEST(CompassSyncPageHandlerMappingTest, GetsAndSetsTheMapping) {
  base::test::TaskEnvironment task_environment;
  MappingController controller;
  FakePage page;
  mojo::Remote<sync::mojom::PageHandler> remote;
  CompassSyncPageHandler handler(
      remote.BindNewPipeAndPassReceiver(), page.BindAndPassRemote(),
      &controller,
      base::BindRepeating([](base::OnceCallback<void(bool)> done) {
        std::move(done).Run(false);
      }));

  base::test::TestFuture<sync::mojom::ProfileMappingPtr> mapping;
  remote->GetProfileMapping(mapping.GetCallback());
  const sync::mojom::ProfileMappingPtr& got = mapping.Get();
  EXPECT_TRUE(got->needed);
  ASSERT_EQ(got->chain.size(), 2u);
  EXPECT_EQ(got->chain[0]->name, "Personal");
  EXPECT_TRUE(got->chain[0]->home);
  ASSERT_EQ(got->local.size(), 1u);
  EXPECT_EQ(got->local[0]->id, "Profile 1");
  ASSERT_EQ(got->proposed.size(), 1u);
  EXPECT_EQ(got->proposed[0]->chain_uuid, "u-work");

  std::vector<sync::mojom::ProfileMappingEntryPtr> entries;
  entries.push_back(sync::mojom::ProfileMappingEntry::New("Profile 1", ""));
  base::test::TestFuture<bool, const std::string&> done;
  remote->SetProfileMapping(std::move(entries), done.GetCallback());
  EXPECT_TRUE(done.Get<0>());
  EXPECT_EQ(controller.mapping(),
            (sync::ProfileMapping{{"Profile 1", std::string()}}));
}

// The defaults of controllers that don't map profiles.
TEST(CompassSyncPageHandlerMappingTest, NothingToMapByDefault) {
  base::test::TaskEnvironment task_environment;
  FakeController controller;
  EXPECT_FALSE(controller.GetProfileMapping().needed);
  base::test::TestFuture<bool, std::string> done;
  controller.SetProfileMapping({}, done.GetCallback());
  EXPECT_FALSE(done.Get<0>());
}

}  // namespace
}  // namespace compass
