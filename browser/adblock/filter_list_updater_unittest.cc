// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/filter_list_updater.h"

#include <map>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/simple_test_clock.h"
#include "base/test/task_environment.h"
#include "base/threading/thread_restrictions.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/adblock/filter_lists.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/prefs/testing_pref_service.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::adblock {
namespace {

constexpr char kEasyListUrl[] = "https://easylist.to/easylist/easylist.txt";
constexpr char kList[] = "[Adblock Plus 2.0]\n! Title: EasyList\n||ads^\n";

class FilterListUpdaterTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    RegisterAdBlockLocalStatePrefs(prefs_.registry());
    // Only EasyList on, to keep the requests to one.
    SetFilterListEnabled(&prefs_, kEasyPrivacyId, false);
    SetFilterListEnabled(&prefs_, kCookiesId, false);
    clock_.SetNow(base::Time::Now());
    updater_ = std::make_unique<FilterListUpdater>(
        &prefs_, dir_.GetPath(),
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_),
        base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()}),
        &clock_,
        base::BindLambdaForTesting([this](const std::string& id,
                                          bool changed) {
          done_[id] = changed;
        }));
  }

  // Runs an update until every due list is checked.
  void Update(bool force) {
    base::RunLoop run_loop;
    updater_->Update(force, run_loop.QuitClosure());
    run_loop.Run();
  }

  std::string ListText(const std::string& id) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string text;
    base::ReadFileToString(GetListTextPath(dir_.GetPath(), id), &text);
    return text;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  TestingPrefServiceSimple prefs_;
  base::SimpleTestClock clock_;
  network::TestURLLoaderFactory url_loader_factory_;
  std::unique_ptr<FilterListUpdater> updater_;
  std::map<std::string, bool> done_;
};

TEST_F(FilterListUpdaterTest, DownloadsAndRecordsHeaders) {
  auto head = network::CreateURLResponseHead(net::HTTP_OK);
  head->headers->AddHeader("ETag", "\"v1\"");
  head->headers->AddHeader("Last-Modified", "Mon, 02 Nov 2026 10:00:00 GMT");
  url_loader_factory_.AddResponse(GURL(kEasyListUrl), std::move(head), kList,
                                  network::URLLoaderCompletionStatus());
  Update(/*force=*/false);

  EXPECT_EQ(ListText(kEasyListId), kList);
  EXPECT_TRUE(done_[kEasyListId]);
  FilterListState state = GetFilterListState(&prefs_, kEasyListId);
  EXPECT_EQ(state.etag, "\"v1\"");
  EXPECT_EQ(state.last_modified, "Mon, 02 Nov 2026 10:00:00 GMT");
  EXPECT_EQ(state.checked, clock_.Now());
  EXPECT_EQ(state.updated, clock_.Now());
  EXPECT_TRUE(state.error.empty());
}

TEST_F(FilterListUpdaterTest, NotModifiedAndDailyGate) {
  url_loader_factory_.AddResponse(kEasyListUrl, kList);
  Update(/*force=*/false);
  const base::Time updated = clock_.Now();

  // Within a day: not asked.
  clock_.Advance(base::Hours(2));
  url_loader_factory_.ClearResponses();
  int requests = 0;
  url_loader_factory_.SetInterceptor(base::BindLambdaForTesting(
      [&](const network::ResourceRequest& request) { ++requests; }));
  Update(/*force=*/false);
  EXPECT_EQ(requests, 0);

  // A day later: asked, with the copy's ETag, and not modified.
  clock_.Advance(base::Hours(23));
  std::string if_none_match;
  url_loader_factory_.SetInterceptor(base::BindLambdaForTesting(
      [&](const network::ResourceRequest& request) {
        ++requests;
        if_none_match = request.headers
                            .GetHeader(net::HttpRequestHeaders::kIfNoneMatch)
                            .value_or("");
      }));
  ScopedDictPrefUpdate(&prefs_, prefs::kListState)
      ->FindDict(kEasyListId)
      ->Set("etag", "\"v1\"");
  url_loader_factory_.AddResponse(GURL(kEasyListUrl),
                                  network::CreateURLResponseHead(
                                      net::HTTP_NOT_MODIFIED),
                                  "", network::URLLoaderCompletionStatus());
  Update(/*force=*/false);
  EXPECT_EQ(requests, 1);
  EXPECT_EQ(if_none_match, "\"v1\"");
  EXPECT_FALSE(done_[kEasyListId]);
  FilterListState state = GetFilterListState(&prefs_, kEasyListId);
  EXPECT_EQ(state.checked, clock_.Now());
  EXPECT_EQ(state.updated, updated);
  EXPECT_EQ(ListText(kEasyListId), kList);

  // Forced: asked again at once.
  Update(/*force=*/true);
  EXPECT_EQ(requests, 2);
}

TEST_F(FilterListUpdaterTest, FailureKeepsLastGoodCopy) {
  url_loader_factory_.AddResponse(kEasyListUrl, kList);
  Update(/*force=*/false);

  url_loader_factory_.AddResponse(kEasyListUrl, "<html>Not found</html>",
                                  net::HTTP_NOT_FOUND);
  Update(/*force=*/true);
  EXPECT_EQ(ListText(kEasyListId), kList);
  EXPECT_EQ(GetFilterListState(&prefs_, kEasyListId).error,
            "The server answered 404");

  url_loader_factory_.AddResponse(kEasyListUrl, "<html>A login page</html>");
  Update(/*force=*/true);
  EXPECT_EQ(ListText(kEasyListId), kList);
  EXPECT_EQ(GetFilterListState(&prefs_, kEasyListId).error,
            "The download isn't a filter list");
  EXPECT_FALSE(done_[kEasyListId]);

  // A failed check leaves the last good one's time: the list is asked for
  // again a day after it, and the error goes once that works.
  clock_.Advance(FilterListUpdater::kMaxAge);
  url_loader_factory_.AddResponse(kEasyListUrl, kList);
  Update(/*force=*/false);
  EXPECT_TRUE(GetFilterListState(&prefs_, kEasyListId).error.empty());
}

TEST_F(FilterListUpdaterTest, CustomListTakesItsTitle) {
  SetFilterListEnabled(&prefs_, kEasyListId, false);
  const GURL url("https://lists.test/mine.txt");
  ASSERT_TRUE(ValidateCustomListUrl(&prefs_, url.spec()).empty());
  const std::string id = AddCustomFilterList(&prefs_, url);
  EXPECT_EQ(FindFilterList(&prefs_, id)->title, "lists.test · mine.txt");

  url_loader_factory_.AddResponse(url.spec(), "! Title: My rules\n||x^\n");
  base::RunLoop run_loop;
  updater_->UpdateList(id);
  updater_->Update(/*force=*/false, run_loop.QuitClosure());
  run_loop.Run();
  EXPECT_EQ(FindFilterList(&prefs_, id)->title, "My rules");

  RemoveCustomFilterList(&prefs_, id);
  EXPECT_FALSE(FindFilterList(&prefs_, id));
  EXPECT_FALSE(prefs_.GetDict(prefs::kListState).Find(id));
}

TEST(FilterListsTest, ValidatesCustomUrls) {
  TestingPrefServiceSimple prefs;
  RegisterAdBlockLocalStatePrefs(prefs.registry());
  EXPECT_FALSE(ValidateCustomListUrl(&prefs, "lists.test/x.txt").empty());
  EXPECT_FALSE(ValidateCustomListUrl(&prefs, "http://lists.test/x").empty());
  EXPECT_FALSE(ValidateCustomListUrl(&prefs, kEasyListUrl).empty());
  EXPECT_TRUE(ValidateCustomListUrl(&prefs, " https://lists.test/x ").empty());
}

TEST(FilterListsTest, LooksLikeFilterList) {
  EXPECT_TRUE(LooksLikeFilterList("[Adblock Plus 2.0]\n||x^"));
  EXPECT_TRUE(LooksLikeFilterList("\xEF\xBB\xBF! Title: x"));
  EXPECT_TRUE(LooksLikeFilterList("\n\n! comment"));
  EXPECT_FALSE(LooksLikeFilterList("<!DOCTYPE html>"));
  EXPECT_FALSE(LooksLikeFilterList("0.0.0.0 ads.example"));
}

}  // namespace
}  // namespace compass::adblock
