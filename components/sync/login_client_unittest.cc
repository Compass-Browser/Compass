// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/login_client.h"

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

constexpr char kServer[] = "https://sync.example.org/v2";
constexpr char kInfoUrl[] = "https://sync.example.org/v2/compass/info";
constexpr char kAccountUrl[] = "https://sync.example.org/v2/compass/account";
constexpr char kLinkUrl[] = "https://sync.example.org/v2/compass/account/link";
constexpr char kLoginUrl[] = "https://sync.example.org/v2/compass/login";
constexpr char kPollUrl[] =
    "https://sync.example.org/v2/compass/login/L1?wait=1";

TEST(LoginClientParseTest, ServerInfo) {
  std::optional<ServerLoginInfo> off = ParseServerLoginInfo(R"({"login":null})");
  ASSERT_TRUE(off);
  EXPECT_FALSE(off->login);
  // A server from before sign-in may say nothing about it.
  ASSERT_TRUE(ParseServerLoginInfo("{}"));
  EXPECT_FALSE(ParseServerLoginInfo("{}")->login);

  std::optional<ServerLoginInfo> on = ParseServerLoginInfo(
      R"({"login":{"type":"oidc","name":"Acme SSO"},"later":1})");
  ASSERT_TRUE(on);
  EXPECT_TRUE(on->login);
  EXPECT_EQ(on->type, "oidc");
  EXPECT_EQ(on->name, "Acme SSO");

  EXPECT_FALSE(ParseServerLoginInfo("404 page not found"));
  EXPECT_FALSE(ParseServerLoginInfo(R"({"login":"yes"})"));
}

TEST(LoginClientParseTest, Account) {
  std::optional<AccountStatus> signed_in = ParseAccountStatus(
      R"({"login_required":false,
          "user":{"name":"Ada Lovelace","email":"ada@example.org"},
          "expires":1800000000000})");
  ASSERT_TRUE(signed_in);
  EXPECT_FALSE(signed_in->login_required);
  ASSERT_TRUE(signed_in->user);
  EXPECT_EQ(signed_in->user->name, "Ada Lovelace");
  EXPECT_EQ(signed_in->user->email, "ada@example.org");
  EXPECT_EQ(signed_in->expires,
            base::Time::FromMillisecondsSinceUnixEpoch(1800000000000));

  std::optional<AccountStatus> required = ParseAccountStatus(
      R"({"login_required":true,"user":null,"expires":0})");
  ASSERT_TRUE(required);
  EXPECT_TRUE(required->login_required);
  EXPECT_FALSE(required->user);
  EXPECT_TRUE(required->expires.is_null());

  EXPECT_FALSE(ParseAccountStatus(R"({"user":null})"));
  EXPECT_FALSE(ParseAccountStatus("[]"));
}

TEST(LoginClientParseTest, LoginRequest) {
  std::optional<LoginRequest> request = ParseLoginRequest(
      R"({"id":"L1","url":"https://sync.example.org/v2/compass/login/L1/start",
          "user_code":"K7F-2QM","expires":1800000600000})");
  ASSERT_TRUE(request);
  EXPECT_EQ(request->id, "L1");
  EXPECT_EQ(request->url,
            GURL("https://sync.example.org/v2/compass/login/L1/start"));
  EXPECT_EQ(request->user_code, "K7F-2QM");
  EXPECT_EQ(request->expires,
            base::Time::FromMillisecondsSinceUnixEpoch(1800000600000));

  // Only a web page opens in the tab.
  EXPECT_FALSE(ParseLoginRequest(
      R"json({"id":"L1","url":"javascript:alert(1)","user_code":"K"})json"));
  EXPECT_FALSE(ParseLoginRequest(R"({"url":"https://a/","user_code":"K"})"));
  EXPECT_FALSE(ParseLoginRequest(R"({"id":"L1","user_code":"K"})"));
}

TEST(LoginClientParseTest, LoginResult) {
  EXPECT_EQ(ParseLoginResult(R"({"status":"pending"})")->state,
            LoginState::kPending);
  EXPECT_EQ(ParseLoginResult(R"({"status":"denied"})")->state,
            LoginState::kDenied);
  EXPECT_EQ(ParseLoginResult(R"({"status":"expired"})")->state,
            LoginState::kExpired);
  std::optional<LoginResult> done = ParseLoginResult(
      R"({"status":"done","user":{"name":"Ada","email":"ada@example.org"}})");
  ASSERT_TRUE(done);
  EXPECT_EQ(done->state, LoginState::kDone);
  ASSERT_TRUE(done->user);
  EXPECT_EQ(done->user->email, "ada@example.org");
  EXPECT_FALSE(ParseLoginResult(R"({"status":"bored"})"));
  EXPECT_FALSE(ParseLoginResult("{}"));
}

TEST(LoginClientParseTest, Urls) {
  EXPECT_EQ(MakeLoginApiUrl(GURL(kServer), "info"), GURL(kInfoUrl));
  EXPECT_EQ(MakeLoginApiUrl(GURL("https://sync.example.org/v2/?x=1#y"),
                            "account/link"),
            GURL(kLinkUrl));
}

TEST(LoginClientParseTest, LoginRequiredReply) {
  EXPECT_TRUE(IsLoginRequiredReply(401, "required"));
  EXPECT_TRUE(IsLoginRequiredReply(401, " Required "));
  EXPECT_FALSE(IsLoginRequiredReply(401, ""));
  EXPECT_FALSE(IsLoginRequiredReply(403, "required"));
}

class LoginClientTest : public testing::Test {
 protected:
  LoginClientTest() { MakeClient("token-1"); }

  void MakeClient(std::optional<std::string> token) {
    client_ = std::make_unique<LoginClient>(
        url_loader_factory_.GetSafeWeakWrapper(), GURL(kServer),
        base::BindLambdaForTesting(
            [token]() -> std::optional<std::string> { return token; }));
  }

  // The last request to |url|, or null.
  const network::ResourceRequest* Pending(const std::string& url) {
    for (auto& pending : *url_loader_factory_.pending_requests()) {
      if (pending.request.url == GURL(url)) {
        return &pending.request;
      }
    }
    return nullptr;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  network::TestURLLoaderFactory url_loader_factory_;
  std::unique_ptr<LoginClient> client_;
};

TEST_F(LoginClientTest, InfoIsUnsigned) {
  std::optional<ServerLoginInfo> info;
  LoginError error = LoginError::kBadReply;
  client_->GetInfo(base::BindLambdaForTesting(
      [&](std::optional<ServerLoginInfo> i, LoginError e) {
        info = i;
        error = e;
      }));
  const network::ResourceRequest* request = Pending(kInfoUrl);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->method, "GET");
  EXPECT_FALSE(request->headers.HasHeader("Authorization"));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kInfoUrl, R"({"login":{"type":"oidc","name":"Acme SSO"}})");
  ASSERT_TRUE(info);
  EXPECT_EQ(error, LoginError::kNone);
  EXPECT_TRUE(info->login);
  EXPECT_EQ(info->name, "Acme SSO");
}

TEST_F(LoginClientTest, AnOlderServerHasNoLogin) {
  std::optional<ServerLoginInfo> info;
  LoginError error = LoginError::kBadReply;
  client_->GetInfo(base::BindLambdaForTesting(
      [&](std::optional<ServerLoginInfo> i, LoginError e) {
        info = i;
        error = e;
      }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kInfoUrl, "404 page not found", net::HTTP_NOT_FOUND);
  ASSERT_TRUE(info);
  EXPECT_FALSE(info->login);
  EXPECT_EQ(error, LoginError::kNone);

  // The network isn't an answer.
  client_->GetInfo(base::BindLambdaForTesting(
      [&](std::optional<ServerLoginInfo> i, LoginError e) {
        info = i;
        error = e;
      }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kInfoUrl, "", net::HTTP_BAD_GATEWAY);
  EXPECT_FALSE(info);
  EXPECT_EQ(error, LoginError::kNetwork);
}

TEST_F(LoginClientTest, AccountIsSigned) {
  std::optional<AccountStatus> account;
  LoginError error = LoginError::kBadReply;
  client_->GetAccount(base::BindLambdaForTesting(
      [&](std::optional<AccountStatus> a, LoginError e) {
        account = a;
        error = e;
      }));
  const network::ResourceRequest* request = Pending(kAccountUrl);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->headers.GetHeader("Authorization"), "Bearer token-1");
  url_loader_factory_.SimulateResponseForPendingRequest(
      kAccountUrl,
      R"({"login_required":false,"user":{"name":"Ada","email":"a@x.org"},)"
      R"("expires":0})");
  ASSERT_TRUE(account);
  EXPECT_EQ(error, LoginError::kNone);
  EXPECT_FALSE(account->login_required);
  EXPECT_EQ(account->user->name, "Ada");
}

TEST_F(LoginClientTest, AccountRefused) {
  std::optional<AccountStatus> account;
  LoginError error = LoginError::kNone;
  client_->GetAccount(base::BindLambdaForTesting(
      [&](std::optional<AccountStatus> a, LoginError e) {
        account = a;
        error = e;
      }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kAccountUrl, R"({"error":"unauthorized"})", net::HTTP_UNAUTHORIZED);
  EXPECT_FALSE(account);
  EXPECT_EQ(error, LoginError::kUnauthorized);
}

TEST_F(LoginClientTest, NoTokenNoRequest) {
  MakeClient(std::nullopt);
  LoginError error = LoginError::kNone;
  client_->GetAccount(base::BindLambdaForTesting(
      [&](std::optional<AccountStatus>, LoginError e) { error = e; }));
  EXPECT_EQ(error, LoginError::kUnauthorized);
  EXPECT_EQ(url_loader_factory_.NumPending(), 0);
}

TEST_F(LoginClientTest, StartLogin) {
  std::optional<LoginRequest> login;
  LoginError error = LoginError::kBadReply;
  client_->StartLogin(
      "Ada's MacBook", base::BindLambdaForTesting(
                           [&](std::optional<LoginRequest> r, LoginError e) {
                             login = r;
                             error = e;
                           }));
  const network::ResourceRequest* request = Pending(kLoginUrl);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->method, "POST");
  EXPECT_EQ(request->headers.GetHeader("Authorization"), "Bearer token-1");
  ASSERT_TRUE(request->request_body);
  const std::string body(network::GetUploadData(*request));
  std::optional<base::DictValue> json = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
  ASSERT_TRUE(json);
  EXPECT_EQ(*json->FindString("device_name"), "Ada's MacBook");
  EXPECT_EQ(*json->FindString("return"), "tab");
  url_loader_factory_.SimulateResponseForPendingRequest(
      kLoginUrl,
      R"({"id":"L1","url":"https://sync.example.org/v2/compass/login/L1/start",)"
      R"("user_code":"K7F-2QM","expires":0})");
  ASSERT_TRUE(login);
  EXPECT_EQ(error, LoginError::kNone);
  EXPECT_EQ(login->user_code, "K7F-2QM");
}

TEST_F(LoginClientTest, StartLoginOnAServerWithout) {
  LoginError error = LoginError::kNone;
  client_->StartLogin("Mac", base::BindLambdaForTesting(
                                 [&](std::optional<LoginRequest> r,
                                     LoginError e) { error = e; }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kLoginUrl, R"({"error":"login_disabled"})", net::HTTP_NOT_FOUND);
  EXPECT_EQ(error, LoginError::kLoginDisabled);

  client_->StartLogin("Mac", base::BindLambdaForTesting(
                                 [&](std::optional<LoginRequest> r,
                                     LoginError e) { error = e; }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kLoginUrl, "404 page not found", net::HTTP_NOT_FOUND);
  EXPECT_EQ(error, LoginError::kNotFound);

  // The identity provider isn't reachable yet.
  client_->StartLogin("Mac", base::BindLambdaForTesting(
                                 [&](std::optional<LoginRequest> r,
                                     LoginError e) { error = e; }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kLoginUrl, R"({"error":"login_unavailable"})",
      net::HTTP_SERVICE_UNAVAILABLE);
  EXPECT_EQ(error, LoginError::kLoginUnavailable);

  client_->StartLogin("Mac", base::BindLambdaForTesting(
                                 [&](std::optional<LoginRequest> r,
                                     LoginError e) { error = e; }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kLoginUrl, "", net::HTTP_TOO_MANY_REQUESTS);
  EXPECT_EQ(error, LoginError::kRateLimited);
}

TEST_F(LoginClientTest, LinkSendsBothTokens) {
  LoginError error = LoginError::kBadReply;
  client_->Link("profile-token", base::BindLambdaForTesting(
                                     [&](LoginError e) { error = e; }));
  const network::ResourceRequest* request = Pending(kLinkUrl);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->method, "POST");
  EXPECT_EQ(request->headers.GetHeader("Authorization"), "Bearer token-1");
  EXPECT_EQ(request->headers.GetHeader("X-Compass-Linked-Token"),
            "profile-token");
  url_loader_factory_.SimulateResponseForPendingRequest(kLinkUrl, "",
                                                        net::HTTP_NO_CONTENT);
  EXPECT_EQ(error, LoginError::kNone);
}

TEST_F(LoginClientTest, SignOut) {
  LoginError error = LoginError::kBadReply;
  client_->SignOut(base::BindLambdaForTesting([&](LoginError e) { error = e; }));
  const network::ResourceRequest* request = Pending(kAccountUrl);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->method, "DELETE");
  url_loader_factory_.SimulateResponseForPendingRequest(kAccountUrl, "",
                                                        net::HTTP_NO_CONTENT);
  EXPECT_EQ(error, LoginError::kNone);
}

// Long polls one after the other: pending, then a network error (retried
// after a pause), then done.
TEST_F(LoginClientTest, WaitsUntilDone) {
  std::optional<LoginResult> result;
  client_->WaitForLogin(
      "L1", base::Time::Now() + base::Minutes(10),
      base::BindLambdaForTesting([&](LoginResult r) { result = r; }));
  const network::ResourceRequest* request = Pending(kPollUrl);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->headers.GetHeader("Authorization"), "Bearer token-1");

  // The server held the poll for its 30 s.
  task_environment_.FastForwardBy(base::Seconds(30));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kPollUrl, R"({"status":"pending"})");
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(result);
  ASSERT_EQ(url_loader_factory_.NumPending(), 1);

  url_loader_factory_.SimulateResponseForPendingRequest(kPollUrl, "",
                                                        net::HTTP_BAD_GATEWAY);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(result);
  EXPECT_EQ(url_loader_factory_.NumPending(), 0);
  task_environment_.FastForwardBy(LoginClient::kPollRetryDelay);
  ASSERT_EQ(url_loader_factory_.NumPending(), 1);

  url_loader_factory_.SimulateResponseForPendingRequest(
      kPollUrl,
      R"({"status":"done","user":{"name":"Ada","email":"ada@example.org"}})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, LoginState::kDone);
  EXPECT_EQ(result->user->email, "ada@example.org");
}

// A server that answers "pending" at once isn't asked in a tight loop.
TEST_F(LoginClientTest, QuickPendingWaits) {
  std::optional<LoginResult> result;
  client_->WaitForLogin(
      "L1", base::Time(),
      base::BindLambdaForTesting([&](LoginResult r) { result = r; }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kPollUrl, R"({"status":"pending"})");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(url_loader_factory_.NumPending(), 0);
  task_environment_.FastForwardBy(base::Seconds(1));
  EXPECT_EQ(url_loader_factory_.NumPending(), 1);
  url_loader_factory_.SimulateResponseForPendingRequest(
      kPollUrl, R"({"status":"denied"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, LoginState::kDenied);
}

TEST_F(LoginClientTest, ExpiresWithoutAnswer) {
  std::optional<LoginResult> result;
  client_->WaitForLogin(
      "L1", base::Time::Now() + base::Seconds(2),
      base::BindLambdaForTesting([&](LoginResult r) { result = r; }));
  url_loader_factory_.SimulateResponseForPendingRequest(kPollUrl, "",
                                                        net::HTTP_BAD_GATEWAY);
  task_environment_.FastForwardBy(LoginClient::kPollRetryDelay);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, LoginState::kExpired);
  EXPECT_EQ(url_loader_factory_.NumPending(), 0);
}

// The server restarted and forgot the login.
TEST_F(LoginClientTest, AnUnknownLoginExpired) {
  std::optional<LoginResult> result;
  client_->WaitForLogin(
      "L1", base::Time(),
      base::BindLambdaForTesting([&](LoginResult r) { result = r; }));
  url_loader_factory_.SimulateResponseForPendingRequest(
      kPollUrl, R"({"error":"not_found"})", net::HTTP_NOT_FOUND);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, LoginState::kExpired);
}

TEST_F(LoginClientTest, DestroyingCancels) {
  bool called = false;
  client_->WaitForLogin("L1", base::Time(),
                        base::BindLambdaForTesting(
                            [&](LoginResult) { called = true; }));
  client_->GetAccount(base::BindLambdaForTesting(
      [&](std::optional<AccountStatus>, LoginError) { called = true; }));
  EXPECT_EQ(url_loader_factory_.NumPending(), 2);
  client_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(called);
}

}  // namespace
}  // namespace compass::sync
