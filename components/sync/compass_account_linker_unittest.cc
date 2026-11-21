// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_account_linker.h"

#include <optional>
#include <string>

#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

constexpr char kServer[] = "https://sync.example.org/v2";
constexpr char kLinkUrl[] = "https://sync.example.org/v2/compass/account/link";

TEST(CompassAccountLinkerTest, UrlAndResults) {
  EXPECT_EQ(GURL(kLinkUrl), MakeAccountLinkUrl(GURL(kServer)));
  EXPECT_EQ(GURL(kLinkUrl),
            MakeAccountLinkUrl(GURL("https://sync.example.org/v2/?x=1#y")));
  using Result = AccountLinker::Result;
  EXPECT_EQ(Result::kLinked, AccountLinker::ResultOf(204));
  // Servers without linking: nothing more to do.
  EXPECT_EQ(Result::kLinked, AccountLinker::ResultOf(404));
  EXPECT_EQ(Result::kLinked, AccountLinker::ResultOf(405));
  EXPECT_EQ(Result::kRefused, AccountLinker::ResultOf(401));
  EXPECT_EQ(Result::kFailed, AccountLinker::ResultOf(429));
  EXPECT_EQ(Result::kFailed, AccountLinker::ResultOf(0));
}

class AccountLinkerTest : public testing::Test {
 protected:
  std::optional<AccountLinker::Result> LinkOnce(AccountLinker& linker) {
    std::optional<AccountLinker::Result> result;
    linker.Link("home-token", "profile-token",
                base::BindLambdaForTesting(
                    [&](AccountLinker::Result r) { result = r; }));
    base::RunLoop().RunUntilIdle();
    return result;
  }

  base::test::TaskEnvironment task_environment_;
  network::TestURLLoaderFactory url_loader_factory_;
};

TEST_F(AccountLinkerTest, SendsBothTokens) {
  AccountLinker linker(url_loader_factory_.GetSafeWeakWrapper(), GURL(kServer));
  url_loader_factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        EXPECT_EQ(GURL(kLinkUrl), request.url);
        EXPECT_EQ("POST", request.method);
        EXPECT_EQ("Bearer home-token",
                  request.headers.GetHeader("Authorization"));
        EXPECT_EQ("profile-token",
                  request.headers.GetHeader(kLinkedTokenHeader));
        EXPECT_EQ(network::mojom::CredentialsMode::kOmit,
                  request.credentials_mode);
      }));
  url_loader_factory_.AddResponse(kLinkUrl, "", net::HTTP_NO_CONTENT);
  EXPECT_EQ(AccountLinker::Result::kLinked, LinkOnce(linker));
  EXPECT_FALSE(linker.busy());

  url_loader_factory_.AddResponse(kLinkUrl, "{}", net::HTTP_UNAUTHORIZED);
  EXPECT_EQ(AccountLinker::Result::kRefused, LinkOnce(linker));
  url_loader_factory_.AddResponse(
      GURL(kLinkUrl), network::mojom::URLResponseHead::New(), "",
      network::URLLoaderCompletionStatus(net::ERR_CONNECTION_REFUSED));
  EXPECT_EQ(AccountLinker::Result::kFailed, LinkOnce(linker));
}

}  // namespace
}  // namespace compass::sync
