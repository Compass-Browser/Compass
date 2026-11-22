// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/adblock_service.h"

#include <memory>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/simple_test_clock.h"
#include "base/test/task_environment.h"
#include "base/threading/thread_restrictions.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/adblock/filter_lists.h"
#include "components/prefs/testing_pref_service.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::adblock {
namespace {

class AdBlockServiceTest : public testing::Test,
                           public AdBlockService::Observer {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    RegisterAdBlockLocalStatePrefs(prefs_.registry());
    clock_.SetNow(base::Time::Now());
  }

  void TearDown() override {
    if (service_) {
      service_->RemoveObserver(this);
    }
  }

  void CreateService() {
    service_ = std::make_unique<AdBlockService>(
        &prefs_, dir_.GetPath(),
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_),
        /*ruleset_service=*/nullptr, &clock_);
    service_->AddObserver(this);
  }

  // AdBlockService::Observer:
  void OnFilterListsChanged() override { ++changes_; }

  // The ruleset files the service built.
  int UnindexedFiles() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    base::FileEnumerator files(
        GetAdBlockDir(dir_.GetPath()).AppendASCII("Unindexed"), false,
        base::FileEnumerator::FILES);
    int count = 0;
    for (base::FilePath file = files.Next(); !file.empty();
         file = files.Next()) {
      ++count;
    }
    return count;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir dir_;
  TestingPrefServiceSimple prefs_;
  base::SimpleTestClock clock_;
  network::TestURLLoaderFactory url_loader_factory_;
  std::unique_ptr<AdBlockService> service_;
  int changes_ = 0;
};

TEST_F(AdBlockServiceTest, Defaults) {
  CreateService();
  EXPECT_TRUE(service_->IsEnabled());
  std::vector<AdBlockService::ListInfo> lists = service_->GetLists();
  ASSERT_EQ(lists.size(), 3u);
  EXPECT_EQ(lists[0].list.id, kEasyListId);
  EXPECT_EQ(lists[1].list.id, kEasyPrivacyId);
  EXPECT_EQ(lists[2].list.id, kCookiesId);
  for (const auto& info : lists) {
    EXPECT_TRUE(info.list.enabled);
    EXPECT_TRUE(info.list.builtin);
  }
}

TEST_F(AdBlockServiceTest, StartBuildsAndDownloadsLater) {
  CreateService();
  service_->Start();
  task_environment_.RunUntilIdle();
  // An empty ruleset at once, so nothing published earlier keeps blocking.
  EXPECT_EQ(UnindexedFiles(), 1);
  EXPECT_EQ(url_loader_factory_.NumPending(), 0);

  task_environment_.FastForwardBy(AdBlockService::kFirstUpdateDelay);
  EXPECT_EQ(url_loader_factory_.NumPending(), 1);  // One list at a time.
  EXPECT_TRUE(service_->IsUpdating());
}

TEST_F(AdBlockServiceTest, NoDownloadsWhileOff) {
  prefs_.SetBoolean(prefs::kEnabled, false);
  CreateService();
  service_->Start();
  task_environment_.FastForwardBy(AdBlockService::kUpdateInterval * 2);
  EXPECT_EQ(url_loader_factory_.NumPending(), 0);

  service_->SetEnabled(true);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(url_loader_factory_.NumPending(), 1);
}

TEST_F(AdBlockServiceTest, ListChangesRebuildOnce) {
  CreateService();
  service_->Start();
  task_environment_.RunUntilIdle();
  changes_ = 0;

  service_->SetListTextForTesting(kEasyListId, "||ads.test^\n",
                                  base::OnceClosure());
  task_environment_.RunUntilIdle();
  service_->SetListEnabled(kCookiesId, false);
  service_->SetListEnabled(kEasyPrivacyId, false);
  EXPECT_GE(changes_, 2);
  task_environment_.FastForwardBy(AdBlockService::kRebuildDelay);
  EXPECT_EQ(UnindexedFiles(), 1);
  EXPECT_EQ(GetFilterListState(&prefs_, kEasyListId).url_rules, 1);
}

TEST_F(AdBlockServiceTest, AddAndRemoveCustomList) {
  CreateService();
  EXPECT_FALSE(service_->AddList("http://lists.test/x.txt").empty());
  EXPECT_TRUE(service_->AddList("https://lists.test/x.txt").empty());
  std::vector<AdBlockService::ListInfo> lists = service_->GetLists();
  ASSERT_EQ(lists.size(), 4u);
  EXPECT_FALSE(lists[3].list.builtin);
  // Asked for at once.
  task_environment_.RunUntilIdle();
  EXPECT_EQ(url_loader_factory_.NumPending(), 1);

  service_->RemoveList(lists[3].list.id);
  service_->RemoveList(kEasyListId);  // Built-ins stay.
  EXPECT_EQ(service_->GetLists().size(), 3u);
}

}  // namespace
}  // namespace compass::adblock
