// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_device_remover.h"

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "compass/components/sync/device_state.h"
#include "net/http/http_response_headers.h"
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
constexpr char kDeviceUrl[] =
    "https://sync.example.org/v2/compass/devices/other-guid?client_id=my-guid";

scoped_refptr<net::HttpResponseHeaders> Headers(
    int status,
    std::initializer_list<std::pair<std::string, std::string>> extra = {}) {
  auto headers = net::HttpResponseHeaders::Builder(
                     net::HttpVersion(1, 1), base::NumberToString(status))
                     .Build();
  for (const auto& [name, value] : extra) {
    headers->AddHeader(name, value);
  }
  return headers;
}

TEST(CompassDeviceRemoverTest, Url) {
  EXPECT_EQ(GURL(kDeviceUrl),
            MakeDeviceUrl(GURL(kServer), "other-guid", "my-guid"));
  EXPECT_EQ(GURL(kDeviceUrl),
            MakeDeviceUrl(GURL("https://sync.example.org/v2/?x=1#y"),
                          "other-guid", "my-guid"));
  // Cache GUIDs are base64: '/', '+' and '=' are escaped.
  EXPECT_EQ(
      "https://sync.example.org/v2/compass/devices/"
      "a%2Fb%2Bc%3D%3D?client_id=d%2Fe%2Bf%3D",
      MakeDeviceUrl(GURL(kServer), "a/b+c==", "d/e+f=").spec());
}

TEST(CompassDeviceRemoverTest, RemovedReply) {
  EXPECT_TRUE(IsDeviceRemovedReply(403, "removed"));
  EXPECT_TRUE(IsDeviceRemovedReply(403, " Removed "));
  EXPECT_FALSE(IsDeviceRemovedReply(403, ""));
  EXPECT_FALSE(IsDeviceRemovedReply(401, "removed"));
  EXPECT_FALSE(IsDeviceRemovedReply(200, "removed"));

  EXPECT_FALSE(IsDeviceRemovedResponse(nullptr));
  EXPECT_TRUE(IsDeviceRemovedResponse(
      Headers(403, {{kDeviceStateHeader, kDeviceStateRemoved}}).get()));
  EXPECT_FALSE(IsDeviceRemovedResponse(Headers(403).get()));
}

TEST(CompassDeviceRemoverTest, StatusMapping) {
  using Result = DeviceRemover::Result;
  EXPECT_EQ(Result::kFailed, DeviceRemover::ResultOf(nullptr));
  EXPECT_EQ(Result::kRemoved, DeviceRemover::ResultOf(Headers(204).get()));
  EXPECT_EQ(Result::kRemoved, DeviceRemover::ResultOf(Headers(200).get()));
  EXPECT_EQ(Result::kThisDevice, DeviceRemover::ResultOf(Headers(409).get()));
  EXPECT_EQ(Result::kFailed, DeviceRemover::ResultOf(Headers(400).get()));
  EXPECT_EQ(Result::kUnauthorized,
            DeviceRemover::ResultOf(Headers(401).get()));
  EXPECT_EQ(Result::kLoginRequired,
            DeviceRemover::ResultOf(
                Headers(401, {{"X-Compass-Login", "https://id.example"}})
                    .get()));
  EXPECT_EQ(Result::kDeviceRemoved,
            DeviceRemover::ResultOf(
                Headers(403, {{kDeviceStateHeader, kDeviceStateRemoved}})
                    .get()));
  EXPECT_EQ(Result::kFailed, DeviceRemover::ResultOf(Headers(403).get()));
  EXPECT_EQ(Result::kUnsupported, DeviceRemover::ResultOf(Headers(404).get()));
  EXPECT_EQ(Result::kUnsupported, DeviceRemover::ResultOf(Headers(405).get()));
  EXPECT_EQ(Result::kFailed, DeviceRemover::ResultOf(Headers(500).get()));
}

class DeviceRemoverTest : public testing::Test {
 protected:
  DeviceRemover MakeRemover(std::optional<std::string> token) {
    return DeviceRemover(
        url_loader_factory_.GetSafeWeakWrapper(), GURL(kServer),
        base::BindLambdaForTesting(
            [token]() -> std::optional<std::string> { return token; }));
  }

  void Respond(int status,
               std::initializer_list<std::pair<std::string, std::string>>
                   extra = {}) {
    auto head = network::CreateURLResponseHead(
        static_cast<net::HttpStatusCode>(status));
    for (const auto& [name, value] : extra) {
      head->headers->AddHeader(name, value);
    }
    url_loader_factory_.AddResponse(GURL(kDeviceUrl), std::move(head),
                                    status == 204 ? "" : "{}",
                                    network::URLLoaderCompletionStatus());
  }

  std::optional<DeviceRemover::Result> RemoveOnce(DeviceRemover& remover) {
    std::optional<DeviceRemover::Result> result;
    remover.Remove("other-guid", "my-guid",
                   base::BindLambdaForTesting(
                       [&](DeviceRemover::Result r) { result = r; }));
    base::RunLoop().RunUntilIdle();
    return result;
  }

  base::test::TaskEnvironment task_environment_;
  network::TestURLLoaderFactory url_loader_factory_;
};

TEST_F(DeviceRemoverTest, SendsASignedDelete) {
  DeviceRemover remover = MakeRemover("token-1");
  url_loader_factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        EXPECT_EQ(GURL(kDeviceUrl), request.url);
        EXPECT_EQ("DELETE", request.method);
        EXPECT_EQ("Bearer token-1", request.headers.GetHeader("Authorization"));
        EXPECT_EQ(network::mojom::CredentialsMode::kOmit,
                  request.credentials_mode);
      }));
  Respond(204);
  EXPECT_EQ(DeviceRemover::Result::kRemoved, RemoveOnce(remover));
  EXPECT_EQ(1u, url_loader_factory_.total_requests());
  EXPECT_EQ(0u, remover.pending());
}

TEST_F(DeviceRemoverTest, MapsErrors) {
  DeviceRemover remover = MakeRemover("token-1");
  Respond(409);
  EXPECT_EQ(DeviceRemover::Result::kThisDevice, RemoveOnce(remover));
  Respond(401, {{"X-Compass-Login", "https://id.example"}});
  EXPECT_EQ(DeviceRemover::Result::kLoginRequired, RemoveOnce(remover));
  Respond(401);
  EXPECT_EQ(DeviceRemover::Result::kUnauthorized, RemoveOnce(remover));
  Respond(403, {{kDeviceStateHeader, kDeviceStateRemoved}});
  EXPECT_EQ(DeviceRemover::Result::kDeviceRemoved, RemoveOnce(remover));
  Respond(400);
  EXPECT_EQ(DeviceRemover::Result::kFailed, RemoveOnce(remover));
  Respond(404);
  EXPECT_EQ(DeviceRemover::Result::kUnsupported, RemoveOnce(remover));
}

TEST_F(DeviceRemoverTest, NetworkErrorFails) {
  DeviceRemover remover = MakeRemover("token-1");
  url_loader_factory_.AddResponse(
      GURL(kDeviceUrl), network::mojom::URLResponseHead::New(), "",
      network::URLLoaderCompletionStatus(net::ERR_CONNECTION_REFUSED));
  EXPECT_EQ(DeviceRemover::Result::kFailed, RemoveOnce(remover));
}

TEST_F(DeviceRemoverTest, NoTokenNoRequest) {
  DeviceRemover remover = MakeRemover(std::nullopt);
  EXPECT_EQ(DeviceRemover::Result::kFailed, RemoveOnce(remover));
  EXPECT_EQ(0u, url_loader_factory_.total_requests());
}

TEST_F(DeviceRemoverTest, RequestsOverlap) {
  DeviceRemover remover = MakeRemover("token-1");
  int done = 0;
  remover.Remove("other-guid", "my-guid",
                 base::BindLambdaForTesting([&](DeviceRemover::Result) {
                   ++done;
                 }));
  remover.Remove("third-guid", "my-guid",
                 base::BindLambdaForTesting([&](DeviceRemover::Result) {
                   ++done;
                 }));
  EXPECT_EQ(2u, remover.pending());
  EXPECT_EQ(2, url_loader_factory_.NumPending());
  Respond(204);
  url_loader_factory_.AddResponse(
      "https://sync.example.org/v2/compass/devices/third-guid"
      "?client_id=my-guid",
      "", net::HTTP_NO_CONTENT);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(2, done);
  EXPECT_EQ(0u, remover.pending());
}

}  // namespace
}  // namespace compass::sync
