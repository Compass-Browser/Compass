// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sync/compass_sync_page_handler.h"

#include <string>
#include <utility>
#include <vector>

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
  std::vector<std::string> calls_;
  base::ObserverList<Observer> observers_;
};

class FakePage : public sync::mojom::Page {
 public:
  mojo::PendingRemote<sync::mojom::Page> BindAndPassRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void OnStateChanged(sync::mojom::StatusPtr status,
                      sync::mojom::PairingPtr pairing) override {
    status_ = std::move(status);
    pairing_ = std::move(pairing);
  }
  const sync::mojom::StatusPtr& status() const { return status_; }
  const sync::mojom::PairingPtr& pairing() const { return pairing_; }

 private:
  sync::mojom::StatusPtr status_;
  sync::mojom::PairingPtr pairing_;
  mojo::Receiver<sync::mojom::Page> receiver_{this};
};

class CompassSyncPageHandlerTest : public testing::Test {
 protected:
  CompassSyncPageHandlerTest()
      : handler_(std::make_unique<CompassSyncPageHandler>(
            remote_.BindNewPipeAndPassReceiver(),
            page_.BindAndPassRemote(),
            &controller_)) {}

  base::test::TaskEnvironment task_environment_;
  FakeController controller_;
  FakePage page_;
  mojo::Remote<sync::mojom::PageHandler> remote_;
  std::unique_ptr<CompassSyncPageHandler> handler_;
};

TEST_F(CompassSyncPageHandlerTest, GetState) {
  controller_.Set({sync::SyncState::kOn, "http://h:8295/v2", "Last synced"},
                  {sync::PairingStep::kShowingCode, "7-482913", "", ""});
  base::test::TestFuture<sync::mojom::StatusPtr, sync::mojom::PairingPtr>
      future;
  remote_->GetState(future.GetCallback());
  auto [status, pairing] = future.Take();
  EXPECT_EQ(status->state, sync::mojom::State::kOn);
  EXPECT_EQ(status->server_url, "http://h:8295/v2");
  EXPECT_EQ(status->message, "Last synced");
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
}

TEST_F(CompassSyncPageHandlerTest, StopsObservingWhenGone) {
  EXPECT_TRUE(controller_.HasObservers());
  handler_.reset();
  EXPECT_FALSE(controller_.HasObservers());
}

}  // namespace
}  // namespace compass
