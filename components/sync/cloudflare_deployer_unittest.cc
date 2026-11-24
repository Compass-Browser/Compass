// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/cloudflare_deployer.h"

#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

constexpr char kApi[] = "https://api.example.com/v4";
constexpr char kOwner[] = "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789";
constexpr char kScript[] = "export default {};";

class CloudflareDeployerTest : public testing::Test {
 protected:
  CloudflareDeployerTest()
      : shared_(base::MakeRefCounted<
                network::WeakWrapperSharedURLLoaderFactory>(&factory_)),
        deployer_(shared_, "the-token", kScript, GURL(kApi)) {
    factory_.SetInterceptor(
        base::BindLambdaForTesting([this](const network::ResourceRequest& r) {
          requests_.push_back(r);
        }));
    deployer_.set_health_interval_for_testing(base::Seconds(1));
    deployer_.set_health_timeout_for_testing(base::Seconds(5));
  }

  void Start() {
    deployer_.Deploy(
        kOwner,
        base::BindLambdaForTesting(
            [this](const std::string& line) { progress_.push_back(line); }),
        base::BindLambdaForTesting(
            [this](CloudflareDeployer::Result r) { result_ = std::move(r); }));
  }

  // Answers the one pending request, which must be |method| |url|.
  void Answer(const std::string& method,
              const std::string& url,
              const std::string& body,
              net::HttpStatusCode status = net::HTTP_OK) {
    base::RunLoop().RunUntilIdle();
    ASSERT_EQ(1, factory_.NumPending()) << url;
    const network::ResourceRequest& r = requests_.back();
    EXPECT_EQ(method, r.method) << url;
    EXPECT_EQ(GURL(url), r.url);
    if (r.url.host() == "api.example.com") {
      EXPECT_EQ("Bearer the-token",
                r.headers.GetHeader("Authorization").value_or(""));
    }
    factory_.SimulateResponseForPendingRequest(url, body, status);
    base::RunLoop().RunUntilIdle();
  }

  std::string LastUpload() {
    return network::GetUploadData(requests_.back());
  }

  base::test::TaskEnvironment env_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  network::TestURLLoaderFactory factory_;
  scoped_refptr<network::SharedURLLoaderFactory> shared_;
  CloudflareDeployer deployer_;
  std::vector<network::ResourceRequest> requests_;
  std::vector<std::string> progress_;
  std::optional<CloudflareDeployer::Result> result_;
};

TEST_F(CloudflareDeployerTest, Metadata) {
  std::optional<base::Value> m = base::JSONReader::Read(
      CloudflareDeployer::MakeMetadata(kOwner, /*with_migration=*/true),
      base::JSON_PARSE_RFC);
  ASSERT_TRUE(m && m->is_dict());
  EXPECT_EQ("index.js", *m->GetDict().FindString("main_module"));
  const base::ListValue* bindings = m->GetDict().FindList("bindings");
  ASSERT_TRUE(bindings);
  ASSERT_EQ(4u, bindings->size());
  EXPECT_EQ("CHAIN", *(*bindings)[0].GetDict().FindString("name"));
  // The owner, lower-case as the server compares it.
  EXPECT_EQ(base::ToLowerASCII(kOwner),
            *(*bindings)[3].GetDict().FindString("text"));
  EXPECT_EQ("v1", *m->GetDict().FindStringByDottedPath("migrations.new_tag"));

  m = base::JSONReader::Read(
      CloudflareDeployer::MakeMetadata(kOwner, /*with_migration=*/false),
      base::JSON_PARSE_RFC);
  EXPECT_FALSE(m->GetDict().Find("migrations"));
}

// A new account: no workers.dev subdomain, no scripts.
TEST_F(CloudflareDeployerTest, NewAccount) {
  Start();
  Answer("GET", std::string(kApi) + "/accounts?per_page=50",
         R"({"success":true,"result":[{"id":"acc1","name":"Me"}]})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/subdomain",
         R"({"success":false,"errors":[{"code":10007,"message":"no subdomain"}]})",
         net::HTTP_NOT_FOUND);
  // The name is random: answer whatever was asked.
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ("PUT", requests_.back().method);
  EXPECT_NE(std::string::npos, LastUpload().find("\"subdomain\":\"compass-"));
  factory_.SimulateResponseForPendingRequest(
      std::string(kApi) + "/accounts/acc1/workers/subdomain",
      R"({"success":true,"result":{"subdomain":"compass-abc"}})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/scripts",
         R"({"success":true,"result":[]})");
  base::RunLoop().RunUntilIdle();
  const std::string upload = LastUpload();
  EXPECT_NE(std::string::npos, upload.find("\"new_sqlite_classes\""));
  EXPECT_NE(std::string::npos, upload.find(kScript));
  EXPECT_NE(std::string::npos,
            upload.find("Content-Type: application/javascript+module"));
  Answer("PUT", std::string(kApi) + "/accounts/acc1/workers/scripts/compass-sync",
         R"({"success":true,"result":{}})");
  Answer("POST",
         std::string(kApi) +
             "/accounts/acc1/workers/scripts/compass-sync/subdomain",
         R"({"success":true,"result":{"enabled":true}})");
  Answer("GET", "https://compass-sync.compass-abc.workers.dev/healthz",
         "ok\n");
  ASSERT_TRUE(result_);
  EXPECT_EQ(CloudflareDeployer::Error::kNone, result_->error);
  ASSERT_TRUE(result_->deployment);
  EXPECT_EQ("acc1", result_->deployment->account_id);
  EXPECT_EQ(GURL("https://compass-sync.compass-abc.workers.dev/v2"),
            result_->deployment->server_url);
  EXPECT_FALSE(progress_.empty());
}

// An account that has a "compass-sync" already, and this chain's own script
// from an earlier try: it's updated, without the migration it ran.
TEST_F(CloudflareDeployerTest, ExistingScripts) {
  Start();
  Answer("GET", std::string(kApi) + "/accounts?per_page=50",
         R"({"success":true,"result":[{"id":"acc1"}]})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/subdomain",
         R"({"success":true,"result":{"subdomain":"me"}})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/scripts",
         R"({"success":true,"result":[
              {"id":"compass-sync","migration_tag":"v1"},
              {"id":"compass-sync-abcdef","migration_tag":"v1"}]})");
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(std::string::npos, LastUpload().find("migrations"));
  Answer("PUT",
         std::string(kApi) + "/accounts/acc1/workers/scripts/compass-sync-abcdef",
         R"({"success":true})");
  Answer("POST",
         std::string(kApi) +
             "/accounts/acc1/workers/scripts/compass-sync-abcdef/subdomain",
         R"({"success":true})");
  Answer("GET", "https://compass-sync-abcdef.me.workers.dev/healthz", "ok");
  ASSERT_TRUE(result_);
  EXPECT_EQ(CloudflareDeployer::Error::kNone, result_->error);
}

TEST_F(CloudflareDeployerTest, BadToken) {
  Start();
  Answer("GET", std::string(kApi) + "/accounts?per_page=50",
         R"({"success":false,"errors":[{"code":1000,"message":"Invalid API Token"}]})",
         net::HTTP_UNAUTHORIZED);
  ASSERT_TRUE(result_);
  EXPECT_EQ(CloudflareDeployer::Error::kUnauthorized, result_->error);
  EXPECT_FALSE(CloudflareDeployer::Describe(*result_).empty());
}

TEST_F(CloudflareDeployerTest, ApiRefusalKeepsItsMessage) {
  Start();
  Answer("GET", std::string(kApi) + "/accounts?per_page=50",
         R"({"success":true,"result":[{"id":"acc1"}]})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/subdomain",
         R"({"success":true,"result":{"subdomain":"me"}})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/scripts",
         R"({"success":true,"result":[]})");
  Answer("PUT", std::string(kApi) + "/accounts/acc1/workers/scripts/compass-sync",
         R"({"success":false,"errors":[{"code":10021,"message":"Script too big"}]})",
         net::HTTP_BAD_REQUEST);
  ASSERT_TRUE(result_);
  EXPECT_EQ(CloudflareDeployer::Error::kApi, result_->error);
  EXPECT_EQ("Script too big", result_->detail);
}

// The new address doesn't resolve in time: the deployment is reported, but
// as not reachable.
TEST_F(CloudflareDeployerTest, NotReachable) {
  Start();
  Answer("GET", std::string(kApi) + "/accounts?per_page=50",
         R"({"success":true,"result":[{"id":"acc1"}]})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/subdomain",
         R"({"success":true,"result":{"subdomain":"me"}})");
  Answer("GET", std::string(kApi) + "/accounts/acc1/workers/scripts",
         R"({"success":true,"result":[]})");
  Answer("PUT", std::string(kApi) + "/accounts/acc1/workers/scripts/compass-sync",
         R"({"success":true})");
  Answer("POST",
         std::string(kApi) +
             "/accounts/acc1/workers/scripts/compass-sync/subdomain",
         R"({"success":true})");
  const std::string health = "https://compass-sync.me.workers.dev/healthz";
  for (int i = 0; i < 10 && !result_; ++i) {
    base::RunLoop().RunUntilIdle();
    if (factory_.NumPending()) {
      factory_.SimulateResponseForPendingRequest(
          GURL(health), network::URLLoaderCompletionStatus(net::ERR_NAME_NOT_RESOLVED),
          network::mojom::URLResponseHead::New(), std::string());
    }
    env_.FastForwardBy(base::Seconds(1));
  }
  ASSERT_TRUE(result_);
  EXPECT_EQ(CloudflareDeployer::Error::kNotReachable, result_->error);
  ASSERT_TRUE(result_->deployment);
}

}  // namespace
}  // namespace compass::sync
