// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_fetcher.h"

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "net/http/http_status_code.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

using Result = base::expected<LiveFetchResponse, LiveError>;

class LiveFetcherTest : public testing::Test {
 protected:
  std::unique_ptr<LiveFetcher> MakeFetcher(
      std::vector<std::string> hosts,
      std::vector<std::string> cookie_hosts = {},
      bool granted = false,
      LiveFetcher::Limits limits = LiveFetcher::Limits()) {
    LiveAllowedHosts allowed;
    allowed.hosts = std::move(hosts);
    allowed.cookies = std::move(cookie_hosts);
    return std::make_unique<LiveFetcher>(
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &factory_),
        std::move(allowed), granted, limits);
  }

  Result Fetch(LiveFetcher& fetcher, LiveFetchRequest request) {
    base::test::TestFuture<Result> future;
    fetcher.Fetch(request, future.GetCallback());
    return future.Take();
  }

  static LiveFetchRequest Get(std::string url) {
    LiveFetchRequest request;
    request.url = std::move(url);
    return request;
  }

  base::test::TaskEnvironment task_environment_;
  network::TestURLLoaderFactory factory_;
};

TEST_F(LiveFetcherTest, FetchesAllowedHosts) {
  factory_.AddResponse("https://feed.test/rss", "<rss/>");
  auto fetcher = MakeFetcher({"feed.test"});
  Result result = Fetch(*fetcher, Get("https://feed.test/rss"));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_EQ(result->status, 200);
  EXPECT_EQ(result->body, "<rss/>");
  EXPECT_EQ(result->url, GURL("https://feed.test/rss"));
  ASSERT_EQ(fetcher->fetches().size(), 1u);
  EXPECT_FALSE(fetcher->fetches()[0].cookies);
}

TEST_F(LiveFetcherTest, HttpErrorsAreResponses) {
  factory_.AddResponse("https://feed.test/missing", "gone",
                       net::HTTP_NOT_FOUND);
  auto fetcher = MakeFetcher({"feed.test"});
  Result result = Fetch(*fetcher, Get("https://feed.test/missing"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, 404);
  EXPECT_EQ(result->body, "gone");
}

TEST_F(LiveFetcherTest, RefusesOtherHostsAndRequests) {
  auto fetcher = MakeFetcher({"*.feed.test"});
  EXPECT_EQ(Fetch(*fetcher, Get("https://evil.test/")).error().code,
            "host_not_allowed");
  EXPECT_EQ(Fetch(*fetcher, Get("https://feed.test.evil.test/")).error().code,
            "host_not_allowed");
  EXPECT_EQ(Fetch(*fetcher, Get("file:///etc/passwd")).error().code,
            "host_not_allowed");
  EXPECT_EQ(Fetch(*fetcher, Get("not a url")).error().code, "bad_request");
  LiveFetchRequest put = Get("https://a.feed.test/");
  put.method = "PUT";
  EXPECT_EQ(Fetch(*fetcher, put).error().code, "bad_request");
  // None of them reached the network.
  EXPECT_EQ(factory_.NumPending(), 0);
  EXPECT_TRUE(fetcher->fetches().empty());
}

TEST_F(LiveFetcherTest, DropsTheHostsHeaders) {
  auto fetcher = MakeFetcher({"api.test"});
  LiveFetchRequest request = Get("https://api.test/items");
  request.headers = {{"Authorization", "Bearer t"},
                     {"Cookie", "stolen=1"},
                     {"Origin", "https://evil.test"},
                     {"Sec-Fetch-Site", "none"}};
  base::test::TestFuture<Result> future;
  fetcher->Fetch(request, future.GetCallback());
  ASSERT_EQ(factory_.NumPending(), 1);
  const network::ResourceRequest& sent =
      factory_.GetPendingRequest(0)->request;
  EXPECT_EQ(sent.headers.GetHeader("Authorization"), "Bearer t");
  EXPECT_FALSE(sent.headers.HasHeader("Cookie"));
  EXPECT_FALSE(sent.headers.HasHeader("Origin"));
  EXPECT_FALSE(sent.headers.HasHeader("Sec-Fetch-Site"));
  EXPECT_EQ(sent.credentials_mode, network::mojom::CredentialsMode::kOmit);
  factory_.AddResponse("https://api.test/items", "[]");
  EXPECT_TRUE(future.Take().has_value());
}

TEST_F(LiveFetcherTest, CookiesOnlyForGrantedCookieHosts) {
  auto granted = MakeFetcher({"site.test", "cdn.test"}, {"site.test"}, true);
  auto not_granted = MakeFetcher({"site.test"}, {"site.test"}, false);

  LiveFetchRequest with_cookies = Get("https://site.test/list");
  with_cookies.include_credentials = true;
  LiveFetchRequest other = Get("https://cdn.test/x");
  other.include_credentials = true;

  for (auto [fetcher, request, expected] :
       {std::tuple{granted.get(), with_cookies, true},
        std::tuple{granted.get(), other, false},
        std::tuple{not_granted.get(), with_cookies, false}}) {
    base::test::TestFuture<Result> future;
    fetcher->Fetch(request, future.GetCallback());
    ASSERT_EQ(factory_.NumPending(), 1);
    EXPECT_EQ(factory_.GetPendingRequest(0)->request.credentials_mode,
              expected ? network::mojom::CredentialsMode::kInclude
                       : network::mojom::CredentialsMode::kOmit)
        << request.url;
    factory_.AddResponse(request.url, "ok");
    EXPECT_TRUE(future.Take().has_value());
    EXPECT_EQ(fetcher->fetches().back().cookies, expected);
  }
}

TEST_F(LiveFetcherTest, ChecksEveryRedirect) {
  auto fetcher = MakeFetcher({"feed.test"});
  net::RedirectInfo redirect;
  redirect.status_code = 302;
  redirect.new_url = GURL("https://evil.test/");
  redirect.new_method = "GET";
  network::TestURLLoaderFactory::Redirects redirects;
  redirects.emplace_back(redirect, network::mojom::URLResponseHead::New());
  factory_.AddResponse(GURL("https://feed.test/moved"),
                       network::mojom::URLResponseHead::New(), "secret",
                       network::URLLoaderCompletionStatus(), std::move(redirects));
  Result result = Fetch(*fetcher, Get("https://feed.test/moved"));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, "host_not_allowed");
}

TEST_F(LiveFetcherTest, CapsFetchesAndResponses) {
  LiveFetcher::Limits limits;
  limits.max_fetches = 2;
  limits.max_response_bytes = 8;
  auto fetcher = MakeFetcher({"feed.test"}, {}, false, limits);
  factory_.AddResponse("https://feed.test/small", "1234");
  factory_.AddResponse("https://feed.test/big", "123456789");
  EXPECT_TRUE(Fetch(*fetcher, Get("https://feed.test/small")).has_value());
  EXPECT_EQ(Fetch(*fetcher, Get("https://feed.test/big")).error().code,
            "response_too_large");
  EXPECT_EQ(Fetch(*fetcher, Get("https://feed.test/small")).error().code,
            "too_many_fetches");
}

TEST_F(LiveFetcherTest, DecodesTheCharset) {
  auto head = network::CreateURLResponseHead(net::HTTP_OK);
  head->charset = "iso-8859-1";
  factory_.AddResponse(GURL("https://feed.test/latin1"), std::move(head),
                       "caf\xE9", network::URLLoaderCompletionStatus());
  auto fetcher = MakeFetcher({"feed.test"});
  Result result = Fetch(*fetcher, Get("https://feed.test/latin1"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->body, "caf\xC3\xA9");
}

TEST_F(LiveFetcherTest, DestroyingCancels) {
  auto fetcher = MakeFetcher({"feed.test"});
  bool called = false;
  fetcher->Fetch(Get("https://feed.test/slow"),
                 base::BindOnce([](bool* called, Result) { *called = true; },
                                &called));
  fetcher.reset();
  factory_.AddResponse("https://feed.test/slow", "late");
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(called);
}

}  // namespace
}  // namespace compass
