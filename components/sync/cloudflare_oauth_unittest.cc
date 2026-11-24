// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/cloudflare_oauth.h"

#include <optional>
#include <string>

#include "base/base64url.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "crypto/hash.h"
#include "net/base/url_util.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

std::string Query(const GURL& url, const std::string& key) {
  std::string value;
  net::GetValueForKeyInQuery(url, key, &value);
  return value;
}

TEST(CloudflareOAuthTest, Request) {
  const CloudflareAuthRequest r = MakeCloudflareAuthRequest();
  EXPECT_EQ(64u, r.verifier.size());
  EXPECT_GE(r.state.size(), 8u);
  EXPECT_TRUE(r.url.spec().starts_with(
      std::string(kCloudflareAuthorizeUrl) + "?"));
  EXPECT_EQ("code", Query(r.url, "response_type"));
  EXPECT_EQ(kCloudflareClientId, Query(r.url, "client_id"));
  EXPECT_EQ(kCloudflareRedirectUri, Query(r.url, "redirect_uri"));
  EXPECT_EQ(kCloudflareScopes, Query(r.url, "scope"));
  EXPECT_EQ(r.state, Query(r.url, "state"));
  EXPECT_EQ("S256", Query(r.url, "code_challenge_method"));
  std::string challenge;
  base::Base64UrlEncode(crypto::hash::Sha256(r.verifier),
                        base::Base64UrlEncodePolicy::OMIT_PADDING, &challenge);
  EXPECT_EQ(challenge, Query(r.url, "code_challenge"));
  // Each request has its own secrets.
  EXPECT_NE(r.verifier, MakeCloudflareAuthRequest().verifier);
}

TEST(CloudflareOAuthTest, Redirect) {
  EXPECT_TRUE(IsCloudflareRedirect(
      GURL("https://compassbrowser.org/oauth/cloudflare?code=a&state=b")));
  EXPECT_FALSE(IsCloudflareRedirect(
      GURL("http://compassbrowser.org/oauth/cloudflare?code=a")));
  EXPECT_FALSE(IsCloudflareRedirect(
      GURL("https://evil.example/oauth/cloudflare?code=a")));
  EXPECT_FALSE(
      IsCloudflareRedirect(GURL("https://compassbrowser.org/oauth/other")));

  CloudflareAuthReply reply = ParseCloudflareRedirect(
      GURL("https://compassbrowser.org/oauth/cloudflare?code=abc&state=s1234567"),
      "s1234567");
  EXPECT_EQ("abc", reply.code.value_or(""));
  reply = ParseCloudflareRedirect(
      GURL("https://compassbrowser.org/oauth/cloudflare?code=abc&state=other"),
      "s1234567");
  EXPECT_FALSE(reply.code);
  EXPECT_EQ("state", reply.error);
  reply = ParseCloudflareRedirect(
      GURL("https://compassbrowser.org/oauth/cloudflare"
           "?error=access_denied&state=s1234567"),
      "s1234567");
  EXPECT_FALSE(reply.code);
  EXPECT_EQ("access_denied", reply.error);
}

TEST(CloudflareOAuthTest, Exchange) {
  base::test::TaskEnvironment env;
  network::TestURLLoaderFactory factory;
  network::ResourceRequest sent;
  factory.SetInterceptor(base::BindLambdaForTesting(
      [&](const network::ResourceRequest& r) { sent = r; }));
  factory.AddResponse(kCloudflareTokenUrl,
                      R"({"access_token":"tok","token_type":"bearer"})");
  CloudflareTokenExchange exchange(
      base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
          &factory));
  std::optional<std::string> token;
  base::RunLoop loop;
  exchange.Exchange("the code", "the-verifier",
                    base::BindLambdaForTesting([&](std::optional<std::string> t) {
                      token = std::move(t);
                      loop.Quit();
                    }));
  loop.Run();
  EXPECT_EQ("tok", token.value_or(""));
  EXPECT_EQ("POST", sent.method);
  const std::string body = network::GetUploadData(sent);
  EXPECT_NE(std::string::npos, body.find("grant_type=authorization_code"));
  EXPECT_NE(std::string::npos, body.find("code=the+code"));
  EXPECT_NE(std::string::npos, body.find("code_verifier=the-verifier"));
  EXPECT_NE(std::string::npos,
            body.find(std::string("client_id=") + kCloudflareClientId));

  // A refusal: no token.
  factory.ClearResponses();
  factory.AddResponse(kCloudflareTokenUrl, R"({"error":"invalid_grant"})",
                      net::HTTP_BAD_REQUEST);
  base::RunLoop loop2;
  exchange.Exchange("c", "v",
                    base::BindLambdaForTesting([&](std::optional<std::string> t) {
                      token = std::move(t);
                      loop2.Quit();
                    }));
  loop2.Run();
  EXPECT_FALSE(token);
}

}  // namespace
}  // namespace compass::sync
