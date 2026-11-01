// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// History sync under local sync (docs/design/sync.md, "Slice 2b notes"): the
// chromium_src override of data_type.cc and the patches to the history
// controllers, HistoryService and BrowsingHistoryService. Each test checks
// the local-sync case and that upstream behaviour without local sync stays.

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "components/history/core/browser/browsing_history_driver.h"
#include "components/history/core/browser/browsing_history_service.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/history/core/browser/sync/history_data_type_controller.h"
#include "components/history/core/browser/sync/history_delete_directives_data_type_controller.h"
#include "components/history/core/common/pref_names.h"
#include "components/history/core/test/history_service_test_util.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/signin/public/identity_manager/account_managed_status_finder_outcome.h"
#include "components/sync/base/client_tag_hash.h"
#include "components/sync/base/data_type.h"
#include "components/sync/model/sync_change.h"
#include "components/sync/model/sync_data.h"
#include "components/sync/model/syncable_service.h"
#include "components/sync/protocol/entity_specifics.pb.h"
#include "components/sync/protocol/history_delete_directive_specifics.pb.h"
#include "components/sync/service/data_type_controller.h"
#include "components/sync/test/fake_sync_change_processor.h"
#include "components/sync/test/test_data_type_store_service.h"
#include "components/sync/test/test_sync_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

using PreconditionContext = syncer::DataTypeController::PreconditionContext;
using PreconditionState = syncer::DataTypeController::PreconditionState;
using Outcome = signin::AccountManagedStatusFinderOutcome;

// chromium_src/components/sync/base/data_type.cc.
TEST(CompassHistorySyncTypesTest, HistoryEncryptableAndInLocalSync) {
  EXPECT_TRUE(syncer::EncryptableUserTypes().Has(syncer::HISTORY));
  EXPECT_TRUE(
      syncer::EncryptableUserTypes().Has(syncer::HISTORY_DELETE_DIRECTIVES));
  EXPECT_TRUE(syncer::LocalSyncSupportedTypes().Has(syncer::HISTORY));
  EXPECT_TRUE(syncer::LocalSyncSupportedTypes().Has(
      syncer::HISTORY_DELETE_DIRECTIVES));

  // Everything else as upstream has it.
  EXPECT_FALSE(syncer::AlwaysEncryptedUserTypes().Has(syncer::HISTORY));
  EXPECT_TRUE(syncer::EncryptableUserTypes().Has(syncer::BOOKMARKS));
  EXPECT_TRUE(syncer::EncryptableUserTypes().HasAll(
      syncer::AlwaysEncryptedUserTypes()));
  EXPECT_FALSE(syncer::EncryptableUserTypes().Has(syncer::DEVICE_INFO));
  EXPECT_TRUE(syncer::LocalSyncSupportedTypes().Has(syncer::BOOKMARKS));
  EXPECT_FALSE(syncer::LocalSyncSupportedTypes().Has(syncer::USER_EVENTS));
  EXPECT_FALSE(syncer::LocalSyncSupportedTypes().Has(syncer::JOURNEY));
}

// The controllers' preconditions
// (patches/components-history-core-browser-sync-*.patch).
class CompassHistoryControllerTest : public testing::Test {
 protected:
  CompassHistoryControllerTest() {
    prefs_.registry()->RegisterBooleanPref(
        ::prefs::kSavingBrowserHistoryDisabled, false);
  }

  // As Compass runs: local sync (no account, so the managed status stays
  // pending) and the chain's custom passphrase.
  void UseLocalSyncWithPassphrase() {
    sync_service_.SetLocalSyncEnabled(true);
    sync_service_.SetIsUsingExplicitPassphrase(true);
  }

  PreconditionState HistoryState(Outcome status) {
    history::HistoryDataTypeController controller(
        &sync_service_, /*history_service=*/nullptr, &prefs_);
    return controller.GetPreconditionState(PreconditionContext(status));
  }

  PreconditionState DirectivesState(Outcome status) {
    history::HistoryDeleteDirectivesDataTypeController controller(
        base::DoNothing(), &sync_service_, &store_service_,
        /*history_service=*/nullptr, &prefs_);
    return controller.GetPreconditionState(PreconditionContext(status));
  }

  base::test::TaskEnvironment task_environment_;
  TestingPrefServiceSimple prefs_;
  syncer::TestSyncService sync_service_;
  syncer::TestDataTypeStoreService store_service_;
};

TEST_F(CompassHistoryControllerTest, RunUnderLocalSyncWithPassphrase) {
  UseLocalSyncWithPassphrase();
  EXPECT_EQ(HistoryState(Outcome::kPending),
            PreconditionState::kPreconditionsMet);
  EXPECT_EQ(DirectivesState(Outcome::kPending),
            PreconditionState::kPreconditionsMet);
}

TEST_F(CompassHistoryControllerTest, PolicyStillStopsHistoryUnderLocalSync) {
  UseLocalSyncWithPassphrase();
  prefs_.SetBoolean(::prefs::kSavingBrowserHistoryDisabled, true);
  EXPECT_EQ(HistoryState(Outcome::kPending),
            PreconditionState::kMustStopAndClearData);
  EXPECT_EQ(DirectivesState(Outcome::kPending),
            PreconditionState::kMustStopAndClearData);
}

TEST_F(CompassHistoryControllerTest, PassphraseStillStopsHistoryWithAccount) {
  ASSERT_FALSE(sync_service_.IsLocalSyncEnabled());
  sync_service_.SetIsUsingExplicitPassphrase(true);
  EXPECT_EQ(HistoryState(Outcome::kConsumerGmail),
            PreconditionState::kMustStopAndClearData);
  EXPECT_EQ(DirectivesState(Outcome::kConsumerGmail),
            PreconditionState::kMustStopAndClearData);
}

TEST_F(CompassHistoryControllerTest, ManagedStatusStillCountsWithAccount) {
  ASSERT_FALSE(sync_service_.IsLocalSyncEnabled());
  EXPECT_EQ(HistoryState(Outcome::kPending),
            PreconditionState::kMustStopAndKeepData);
  EXPECT_EQ(HistoryState(Outcome::kEnterprise),
            PreconditionState::kMustStopAndClearData);
  EXPECT_EQ(HistoryState(Outcome::kConsumerGmail),
            PreconditionState::kPreconditionsMet);
}

// Delete directives without web history
// (patches/components-history-core-browser-history_service.cc.patch and
// patches/components-history-core-browser-browsing_history_service.cc.patch).
class CompassHistoryDeleteDirectivesTest : public testing::Test {
 protected:
  CompassHistoryDeleteDirectivesTest() {
    EXPECT_TRUE(history_dir_.CreateUniqueTempDir());
    history_service_ =
        history::CreateHistoryService(history_dir_.GetPath(), true);
  }

  ~CompassHistoryDeleteDirectivesTest() override {
    if (processor_) {
      // StopSyncing() deletes the processor.
      processor_ = nullptr;
      history_service_->GetDeleteDirectivesSyncableService()->StopSyncing(
          syncer::HISTORY_DELETE_DIRECTIVES);
    }
    history_service_.reset();
    task_environment_.RunUntilIdle();
  }

  // HISTORY_DELETE_DIRECTIVES starts, with a processor that records what
  // would be committed.
  void StartSyncingDirectives() {
    auto processor = std::make_unique<syncer::FakeSyncChangeProcessor>();
    processor_ = processor.get();
    history_service_->GetDeleteDirectivesSyncableService()
        ->MergeDataAndStartSyncing(syncer::HISTORY_DELETE_DIRECTIVES,
                                   syncer::SyncDataList(),
                                   std::move(processor));
  }

  // The directives committed so far.
  std::vector<sync_pb::HistoryDeleteDirectiveSpecifics> Directives() const {
    std::vector<sync_pb::HistoryDeleteDirectiveSpecifics> result;
    if (!processor_) {
      return result;
    }
    for (const syncer::SyncChange& change : processor_->changes()) {
      EXPECT_EQ(change.change_type(), syncer::SyncChange::ACTION_ADD);
      result.push_back(
          change.sync_data().GetSpecifics().history_delete_directive());
    }
    return result;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir history_dir_;
  std::unique_ptr<history::HistoryService> history_service_;
  raw_ptr<syncer::FakeSyncChangeProcessor> processor_ = nullptr;
};

TEST_F(CompassHistoryDeleteDirectivesTest, TimeRangeWithoutWebHistory) {
  StartSyncingDirectives();
  base::CancelableTaskTracker tracker;
  const base::Time now = base::Time::Now();
  history_service_->DeleteLocalAndRemoteHistoryBetween(
      /*web_history=*/nullptr, now - base::Hours(1), now,
      history::kNoAppIdFilter, base::DoNothing(), &tracker);
  history::BlockUntilHistoryProcessesPendingRequests(history_service_.get());

  const auto directives = Directives();
  ASSERT_EQ(directives.size(), 1u);
  EXPECT_TRUE(directives[0].has_time_range_directive());
}

TEST_F(CompassHistoryDeleteDirectivesTest, UrlWithoutWebHistory) {
  StartSyncingDirectives();
  history_service_->DeleteLocalAndRemoteUrl(
      /*web_history=*/nullptr, GURL("https://example.com/compass-history"));
  history::BlockUntilHistoryProcessesPendingRequests(history_service_.get());

  const auto directives = Directives();
  ASSERT_EQ(directives.size(), 1u);
  ASSERT_TRUE(directives[0].has_url_directive());
  EXPECT_EQ(directives[0].url_directive().url(),
            "https://example.com/compass-history");
}

TEST_F(CompassHistoryDeleteDirectivesTest, NothingWhenDirectivesDontSync) {
  // As upstream: no web history and no running type, no directive (and no
  // crash: the handler answers with an error that's ignored).
  base::CancelableTaskTracker tracker;
  const base::Time now = base::Time::Now();
  history_service_->DeleteLocalAndRemoteHistoryBetween(
      /*web_history=*/nullptr, now - base::Hours(1), now,
      history::kNoAppIdFilter, base::DoNothing(), &tracker);
  history_service_->DeleteLocalAndRemoteUrl(
      /*web_history=*/nullptr, GURL("https://example.com/compass-history"));
  history::BlockUntilHistoryProcessesPendingRequests(history_service_.get());
  EXPECT_TRUE(Directives().empty());
}

class TestBrowsingHistoryDriver : public history::BrowsingHistoryDriver {
 public:
  TestBrowsingHistoryDriver() = default;

  // history::BrowsingHistoryDriver:
  void OnRemoveVisits(
      const std::vector<history::ExpireHistoryArgs>& expire_list) override {}
  bool AllowHistoryDeletions() override { return true; }
  bool ShouldHideWebHistoryUrl(const GURL& url) override { return false; }
  history::WebHistoryService* GetWebHistoryService() override {
    return nullptr;
  }
  void ShouldShowNoticeAboutOtherFormsOfBrowsingHistory(
      const syncer::SyncService* sync_service,
      history::WebHistoryService* history_service,
      base::OnceCallback<void(bool)> callback) override {}
};

// What chrome://history sends when a visit is removed.
std::vector<history::BrowsingHistoryService::HistoryEntry> OneVisit() {
  history::BrowsingHistoryService::HistoryEntry entry;
  entry.url = GURL("https://example.com/compass-history");
  entry.time = base::Time::Now() - base::Minutes(5);
  entry.all_timestamps[entry.url] = std::set<base::Time>{entry.time};
  return {entry};
}

TEST_F(CompassHistoryDeleteDirectivesTest, RemoveVisitsUnderLocalSync) {
  StartSyncingDirectives();
  syncer::TestSyncService sync_service;
  sync_service.SetLocalSyncEnabled(true);
  TestBrowsingHistoryDriver driver;
  history::BrowsingHistoryService service(&driver, history_service_.get(),
                                          &sync_service);
  service.RemoveVisits(OneVisit());
  history::BlockUntilHistoryProcessesPendingRequests(history_service_.get());

  const auto directives = Directives();
  ASSERT_EQ(directives.size(), 1u);
  ASSERT_TRUE(directives[0].has_global_id_directive());
  EXPECT_EQ(directives[0].global_id_directive().global_id_size(), 1);
}

TEST_F(CompassHistoryDeleteDirectivesTest, RemoveVisitsWithoutLocalSync) {
  // As upstream: a signed-in profile without web history (history sync off)
  // makes no directive here.
  StartSyncingDirectives();
  syncer::TestSyncService sync_service;
  ASSERT_FALSE(sync_service.IsLocalSyncEnabled());
  TestBrowsingHistoryDriver driver;
  history::BrowsingHistoryService service(&driver, history_service_.get(),
                                          &sync_service);
  service.RemoveVisits(OneVisit());
  history::BlockUntilHistoryProcessesPendingRequests(history_service_.get());
  EXPECT_TRUE(Directives().empty());
}

// Processed directives at startup
// (patches/components-history-core-browser-sync-delete_directive_handler.*,
// docs/design/sync.md, "Slice 7 notes"): under local sync the young ones
// stay on the server, so a device that joins later still applies them.
class CompassKeptDirectivesTest : public CompassHistoryDeleteDirectivesTest {
 protected:
  CompassKeptDirectivesTest() {
    prefs_.registry()->RegisterBooleanPref(
        ::prefs::kSavingBrowserHistoryDisabled, false);
  }

  static syncer::SyncData TimeRange(const std::string& tag, base::Time end) {
    sync_pb::EntitySpecifics specifics;
    sync_pb::TimeRangeDirective* range =
        specifics.mutable_history_delete_directive()
            ->mutable_time_range_directive();
    range->set_start_time_usec(
        (end - base::Hours(1) - base::Time::UnixEpoch()).InMicroseconds());
    range->set_end_time_usec((end - base::Time::UnixEpoch()).InMicroseconds());
    return syncer::SyncData::CreateRemoteData(
        std::move(specifics), syncer::ClientTagHash::FromHashed(tag));
  }

  // Starts HISTORY_DELETE_DIRECTIVES with a young and an old directive, the
  // controller made with local sync on or off; returns the tags of the
  // directives deleted from the server.
  std::set<std::string> StartWithDirectives(bool local_sync) {
    syncer::TestSyncService sync_service;
    sync_service.SetLocalSyncEnabled(local_sync);
    syncer::TestDataTypeStoreService store_service;
    history::HistoryDeleteDirectivesDataTypeController controller(
        base::DoNothing(), &sync_service, &store_service,
        history_service_.get(), &prefs_);

    const base::Time now = base::Time::Now();
    auto processor = std::make_unique<syncer::FakeSyncChangeProcessor>();
    processor_ = processor.get();
    history_service_->GetDeleteDirectivesSyncableService()
        ->MergeDataAndStartSyncing(
            syncer::HISTORY_DELETE_DIRECTIVES,
            {TimeRange("young", now - base::Days(3)),
             TimeRange("old", now - base::Days(40))},
            std::move(processor));
    history::BlockUntilHistoryProcessesPendingRequests(history_service_.get());
    task_environment_.RunUntilIdle();

    std::set<std::string> deleted;
    for (const syncer::SyncChange& change : processor_->changes()) {
      EXPECT_EQ(change.change_type(), syncer::SyncChange::ACTION_DELETE);
      deleted.insert(change.sync_data().GetClientTagHash().value());
    }
    return deleted;
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(CompassKeptDirectivesTest, YoungDirectivesStayUnderLocalSync) {
  EXPECT_EQ(StartWithDirectives(/*local_sync=*/true),
            std::set<std::string>({"old"}));
}

TEST_F(CompassKeptDirectivesTest, AllDroppedWithoutLocalSync) {
  // As upstream: processed directives leave the server at startup.
  EXPECT_EQ(StartWithDirectives(/*local_sync=*/false),
            std::set<std::string>({"old", "young"}));
}

}  // namespace
}  // namespace compass::sync
