// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/access_client.h"

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

constexpr char kServer[] = "https://compass-sync.me.workers.dev/v2";

TEST(AccessClientTest, ParseInviteLink) {
  std::optional<InviteLink> link = ParseInviteLink(
      "  https://compass-sync.me.workers.dev/invite#abcdefghijklmnopqrstuvwxyz ");
  ASSERT_TRUE(link);
  EXPECT_EQ(GURL(kServer), link->server_url);
  EXPECT_EQ("abcdefghijklmnopqrstuvwxyz", link->code);

  // Under a path prefix, and the code's case doesn't matter.
  link = ParseInviteLink("https://example.org/sync/invite#ABCDEFGHIJ234567");
  ASSERT_TRUE(link);
  EXPECT_EQ(GURL("https://example.org/sync/v2"), link->server_url);
  EXPECT_EQ("abcdefghij234567", link->code);

  EXPECT_FALSE(ParseInviteLink("https://example.org/invite"));
  EXPECT_FALSE(ParseInviteLink("https://example.org/invite#short"));
  EXPECT_FALSE(ParseInviteLink("https://example.org/other#abcdefghijklmnop"));
  EXPECT_FALSE(ParseInviteLink("ftp://example.org/invite#abcdefghijklmnop"));
  EXPECT_FALSE(ParseInviteLink("https://example.org/invite#abcdefghij-klmn"));
  EXPECT_FALSE(ParseInviteLink("7-482913"));
}

class AccessClientRequestTest : public testing::Test {
 protected:
  AccessClientRequestTest()
      : client_(base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
                    &factory_),
                GURL(kServer)) {
    factory_.SetInterceptor(
        base::BindLambdaForTesting([this](const network::ResourceRequest& r) {
          last_ = r;
        }));
  }

  base::test::TaskEnvironment env_;
  network::TestURLLoaderFactory factory_;
  AccessClient client_;
  network::ResourceRequest last_;
};

TEST_F(AccessClientRequestTest, List) {
  factory_.AddResponse(std::string(kServer) + "/compass/access",
                       R"({"owners":["aa"],
                           "members":[{"chain":"bb","name":"Sam",
                                       "added_at":1700000000,"linked":2}],
                           "invites":[{"code":"cc","name":"Kim",
                                       "expires_in":3600}]})");
  std::optional<ServerPeople> people;
  AccessClient::Error error = AccessClient::Error::kFailed;
  base::RunLoop loop;
  client_.List("tok", base::BindLambdaForTesting(
                          [&](std::optional<ServerPeople> p,
                              AccessClient::Error e) {
                            people = std::move(p);
                            error = e;
                            loop.Quit();
                          }));
  loop.Run();
  EXPECT_EQ("Bearer tok", last_.headers.GetHeader("Authorization").value_or(""));
  EXPECT_EQ(AccessClient::Error::kNone, error);
  ASSERT_TRUE(people);
  ASSERT_EQ(1u, people->members.size());
  EXPECT_EQ("Sam", people->members[0].name);
  EXPECT_EQ(2, people->members[0].linked);
  EXPECT_EQ(base::Time::FromSecondsSinceUnixEpoch(1700000000),
            people->members[0].added);
  ASSERT_EQ(1u, people->invites.size());
  EXPECT_EQ("cc", people->invites[0].code);
}

TEST_F(AccessClientRequestTest, ListOnAServerWithoutAccessOrAsAFriend) {
  for (auto [status, want] :
       {std::pair{net::HTTP_NOT_FOUND, AccessClient::Error::kUnsupported},
        std::pair{net::HTTP_FORBIDDEN, AccessClient::Error::kNotOwner}}) {
    factory_.ClearResponses();
    factory_.AddResponse(std::string(kServer) + "/compass/access",
                         R"({"error":"x"})", status);
    AccessClient::Error error = AccessClient::Error::kNone;
    base::RunLoop loop;
    client_.List("tok", base::BindLambdaForTesting(
                            [&](std::optional<ServerPeople> p,
                                AccessClient::Error e) {
                              EXPECT_FALSE(p);
                              error = e;
                              loop.Quit();
                            }));
    loop.Run();
    EXPECT_EQ(want, error);
  }
}

TEST_F(AccessClientRequestTest, CreateInvite) {
  factory_.AddResponse(
      std::string(kServer) + "/compass/access/invites",
      R"({"code":"abc","link":"https://s.example/invite#abc"})",
      net::HTTP_CREATED);
  std::optional<std::string> link;
  base::RunLoop loop;
  client_.CreateInvite("tok", "Sam",
                       base::BindLambdaForTesting(
                           [&](std::optional<std::string> l,
                               AccessClient::Error e) {
                             link = std::move(l);
                             loop.Quit();
                           }));
  loop.Run();
  EXPECT_EQ("POST", last_.method);
  EXPECT_EQ(R"({"name":"Sam"})", network::GetUploadData(last_));
  EXPECT_EQ("https://s.example/invite#abc", link.value_or(""));
}

TEST_F(AccessClientRequestTest, JoinErrors) {
  const std::string url = std::string(kServer) + "/compass/access/join";
  for (auto [status, body, want] :
       {std::tuple{net::HTTP_NO_CONTENT, "", AccessClient::Error::kNone},
        std::tuple{net::HTTP_NOT_FOUND, R"({"error":"no_such_invite"})",
                   AccessClient::Error::kNoSuchInvite},
        std::tuple{net::HTTP_NOT_FOUND, "404 page not found",
                   AccessClient::Error::kUnsupported},
        std::tuple{net::HTTP_GONE, R"({"error":"invite_used_or_expired"})",
                   AccessClient::Error::kInviteGone}}) {
    factory_.ClearResponses();
    factory_.AddResponse(url, body, status);
    AccessClient::Error error = AccessClient::Error::kFailed;
    base::RunLoop loop;
    client_.Join("tok", "code",
                 base::BindLambdaForTesting([&](AccessClient::Error e) {
                   error = e;
                   loop.Quit();
                 }));
    loop.Run();
    EXPECT_EQ(want, error) << status;
    EXPECT_FALSE(AccessClient::Describe(error).empty() &&
                 error != AccessClient::Error::kNone);
  }
}

TEST_F(AccessClientRequestTest, RemoveEscapesTheTarget) {
  factory_.AddResponse(std::string(kServer) + "/compass/access/a%2Fb", "",
                       net::HTTP_NO_CONTENT);
  base::RunLoop loop;
  client_.Remove("tok", "a/b",
                 base::BindLambdaForTesting([&](AccessClient::Error e) {
                   EXPECT_EQ(AccessClient::Error::kNone, e);
                   loop.Quit();
                 }));
  loop.Run();
  EXPECT_EQ("DELETE", last_.method);
}

}  // namespace
}  // namespace compass::sync
