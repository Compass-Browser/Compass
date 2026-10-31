// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_sync_quota.h"

#include <optional>
#include <string>

#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

constexpr char kQuotaUrl[] = "https://sync.example.org/v2/compass/quota";

TEST(CompassSyncQuotaTest, ParsesTheServersAnswer) {
  const std::optional<QuotaStatus> status = ParseQuotaStatus(R"({
      "entities": 1200, "max_entities": 100000,
      "bytes": 3000000000, "max_bytes": 4000000000,
      "history": 30000, "max_history": 30000, "history_days": 14,
      "full": ["history", "something-new"], "full_since": 1793491200,
      "added_later": true})");
  ASSERT_TRUE(status);
  EXPECT_EQ(1200, status->entities);
  EXPECT_EQ(100000, status->max_entities);
  EXPECT_EQ(3000000000, status->bytes);
  EXPECT_EQ(4000000000, status->max_bytes);
  EXPECT_EQ(30000, status->history);
  EXPECT_EQ(30000, status->max_history);
  EXPECT_EQ(14, status->history_days);
  EXPECT_FALSE(status->entities_full);
  EXPECT_FALSE(status->bytes_full);
  EXPECT_TRUE(status->history_full);
  EXPECT_FALSE(status->IsServerFull());

  const std::optional<QuotaStatus> full =
      ParseQuotaStatus(R"({"max_entities": 10, "full": ["entities", "bytes"]})");
  ASSERT_TRUE(full);
  EXPECT_TRUE(full->entities_full);
  EXPECT_TRUE(full->bytes_full);
  EXPECT_TRUE(full->IsServerFull());
}

TEST(CompassSyncQuotaTest, RejectsWhatIsNotAStatus) {
  EXPECT_FALSE(ParseQuotaStatus(""));
  EXPECT_FALSE(ParseQuotaStatus("404 page not found"));
  EXPECT_FALSE(ParseQuotaStatus("[]"));
  EXPECT_FALSE(ParseQuotaStatus(R"({"full": []})"));
  EXPECT_FALSE(ParseQuotaStatus(R"({"max_entities": 1})"));
}

TEST(CompassSyncQuotaTest, Url) {
  EXPECT_EQ(GURL(kQuotaUrl),
            MakeQuotaUrl(GURL("https://sync.example.org/v2")));
  EXPECT_EQ(GURL(kQuotaUrl),
            MakeQuotaUrl(GURL("https://sync.example.org/v2/?x=1#y")));
}

class QuotaFetcherTest : public testing::Test {
 protected:
  std::optional<std::optional<QuotaStatus>> FetchOnce(QuotaFetcher& fetcher) {
    std::optional<std::optional<QuotaStatus>> result;
    EXPECT_TRUE(fetcher.Fetch(base::BindLambdaForTesting(
        [&](std::optional<QuotaStatus> status) { result = status; })));
    base::RunLoop().RunUntilIdle();
    return result;
  }

  QuotaFetcher MakeFetcher(std::optional<std::string> token) {
    return QuotaFetcher(
        url_loader_factory_.GetSafeWeakWrapper(),
        GURL("https://sync.example.org/v2"),
        base::BindLambdaForTesting(
            [token]() -> std::optional<std::string> { return token; }));
  }

  base::test::TaskEnvironment task_environment_;
  network::TestURLLoaderFactory url_loader_factory_;
};

TEST_F(QuotaFetcherTest, SignsAndParses) {
  QuotaFetcher fetcher = MakeFetcher("token-1");
  url_loader_factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        EXPECT_EQ(GURL(kQuotaUrl), request.url);
        EXPECT_EQ("GET", request.method);
        EXPECT_EQ("Bearer token-1", request.headers.GetHeader("Authorization"));
      }));
  url_loader_factory_.AddResponse(
      kQuotaUrl, R"({"max_entities": 100000, "full": ["bytes"]})");
  const auto result = FetchOnce(fetcher);
  ASSERT_TRUE(result);
  ASSERT_TRUE(*result);
  EXPECT_TRUE((*result)->bytes_full);
  EXPECT_FALSE(fetcher.IsFetching());
}

TEST_F(QuotaFetcherTest, AnOlderServerOrAnErrorIsNoAnswer) {
  QuotaFetcher fetcher = MakeFetcher("token-1");
  url_loader_factory_.AddResponse(kQuotaUrl, "404 page not found",
                                  net::HTTP_NOT_FOUND);
  const auto result = FetchOnce(fetcher);
  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
}

TEST_F(QuotaFetcherTest, NoTokenNoRequest) {
  QuotaFetcher fetcher = MakeFetcher(std::nullopt);
  const auto result = FetchOnce(fetcher);
  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
  EXPECT_EQ(0u, url_loader_factory_.total_requests());
}

TEST_F(QuotaFetcherTest, OneAtATime) {
  QuotaFetcher fetcher = MakeFetcher("token-1");
  EXPECT_TRUE(fetcher.Fetch(base::DoNothing()));
  EXPECT_TRUE(fetcher.IsFetching());
  EXPECT_FALSE(fetcher.Fetch(base::DoNothing()));
  EXPECT_EQ(1, url_loader_factory_.NumPending());
}

}  // namespace
}  // namespace compass::sync
