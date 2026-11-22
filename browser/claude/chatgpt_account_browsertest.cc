// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/chatgpt_account.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/base64url.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/synchronization/lock.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/chatgpt_oauth.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_credentials.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/base/url_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"

namespace compass::claude {
namespace {

constexpr char kClientId[] = "oaiapp_test";

std::map<std::string, std::string> FormFields(std::string_view body) {
  std::map<std::string, std::string> fields;
  // A URL's query parses the same way.
  // The iterator keeps a reference to its URL.
  const GURL url(base::StrCat({"http://x/?", body}));
  for (net::QueryIterator it(url); !it.IsAtEnd();
       it.Advance()) {
    fields[std::string(it.GetKey())] = it.GetUnescapedValue();
  }
  return fields;
}

// OpenAI's sign-in service, as far as Compass uses it: the token endpoint
// (code exchange and refresh), discovery and revocation.
class ChatGPTAccountBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &ChatGPTAccountBrowserTest::Serve, base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());
    profile()->GetPrefs()->SetString(prefs::kProvider, "openai");
    account()->SetUrlsForTesting(embedded_test_server()->GetURL("/authorize"),
                                 embedded_test_server()->GetURL("/token"),
                                 embedded_test_server()->GetURL("/discovery"));
  }

  Profile* profile() { return browser()->GetProfile(); }
  ChatGPTAccount* account() { return ChatGPTAccount::ForProfile(profile()); }

  std::string IdToken() {
    base::AutoLock lock(lock_);
    const base::Time now = base::Time::Now();
    std::string payload;
    base::Base64UrlEncode(
        *base::WriteJson(
            base::DictValue()
                .Set("iss", chatgpt::kIssuer)
                .Set("aud", kClientId)
                .Set("nonce", nonce_)
                .Set("sub", "user-1")
                .Set("email", "fox@example.com")
                .Set("iat", now.InSecondsFSinceUnixEpoch())
                .Set("exp", (now + base::Hours(1)).InSecondsFSinceUnixEpoch())),
        base::Base64UrlEncodePolicy::OMIT_PADDING, &payload);
    return base::StrCat({"e30.", payload, ".c2ln"});
  }

  std::unique_ptr<net::test_server::HttpResponse> Serve(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    response->set_content_type("application/json");
    const std::string path(request.GetURL().path());
    base::AutoLock lock(lock_);
    if (path == "/token") {
      std::map<std::string, std::string> fields = FormFields(request.content);
      token_requests_.push_back(fields);
      if (refuse_refresh_ && fields["grant_type"] == "refresh_token") {
        response->set_code(net::HTTP_BAD_REQUEST);
        response->set_content(R"({"error":"refresh_token_reused"})");
        return response;
      }
      ++issued_;
      response->set_content(*base::WriteJson(
          base::DictValue()
              .Set("access_token", base::StrCat({"at", base::NumberToString(
                                                           issued_)}))
              .Set("refresh_token", base::StrCat({"rt", base::NumberToString(
                                                            issued_)}))
              .Set("id_token", id_token_)
              .Set("token_type", "Bearer")
              .Set("expires_in", expires_in_)
              .Set("scope", chatgpt::kScopes)));
      return response;
    }
    if (path == "/discovery") {
      response->set_content(
          R"({"revocation_endpoint":"https://auth.openai.com/revoke"})");
      return response;
    }
    if (path == "/revoke") {
      revoked_.push_back(FormFields(request.content));
      response->set_content("");
      return response;
    }
    return nullptr;
  }

  // Signs in: the "browser" goes straight back to the redirect with a code,
  // as auth.openai.com does once the user approves.
  void SignIn() {
    base::test::TestFuture<std::optional<std::string>> done;
    GURL authorize;
    account()->SignIn(
        base::BindLambdaForTesting([&](const GURL& url) { authorize = url; }),
        done.GetCallback());
    ASSERT_TRUE(base::test::RunUntil([&] { return authorize.is_valid(); }));
    std::map<std::string, std::string> query = FormFields(authorize.query());
    EXPECT_EQ(authorize.path(), "/authorize");
    EXPECT_EQ(query["client_id"],
              profile()->GetPrefs()->GetString(prefs::kChatGPTClientId).empty()
                  ? "dynamic_agent_client"
                  : kClientId);
    EXPECT_EQ(query["code_challenge_method"], "S256");
    EXPECT_FALSE(query["ext_agent_host_id"].empty());
    {
      base::AutoLock lock(lock_);
      nonce_ = query["nonce"];
    }
    std::string id_token = IdToken();
    {
      base::AutoLock lock(lock_);
      id_token_ = id_token;
    }
    const GURL redirect(base::StrCat(
        {query["redirect_uri"], "?code=c0de&state=", query["state"],
         "&client_id=", kClientId, "&scope=chatgpt.tokens.use.direct"}));
    ASSERT_TRUE(redirect.HostIsIPAddress());
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), redirect));
    EXPECT_EQ(done.Get(), std::nullopt);
    content::WebContents* tab =
        browser()->tab_strip_model()->GetActiveWebContents();
    EXPECT_EQ(tab->GetTitle(), u"Signed in to ChatGPT");
  }

  base::expected<std::string, std::string> Token() {
    base::test::TestFuture<base::expected<std::string, std::string>> token;
    account()->GetAccessToken(token.GetCallback());
    return token.Take();
  }

  base::Lock lock_;
  std::string nonce_;
  std::string id_token_;
  int issued_ = 0;
  int expires_in_ = 3600;
  bool refuse_refresh_ = false;
  std::vector<std::map<std::string, std::string>> token_requests_;
  std::vector<std::map<std::string, std::string>> revoked_;
};

IN_PROC_BROWSER_TEST_F(ChatGPTAccountBrowserTest, SignInUseRefreshSignOut) {
  PrefService* prefs = profile()->GetPrefs();
  EXPECT_FALSE(ClaudeCredentials::IsSetUp(*prefs));
  ASSERT_NO_FATAL_FAILURE(SignIn());

  // Registered and signed in; the tokens are stored encrypted.
  EXPECT_EQ(prefs->GetString(prefs::kChatGPTClientId), kClientId);
  EXPECT_EQ(ChatGPTAccount::AccountEmail(*prefs), "fox@example.com");
  EXPECT_TRUE(UsesChatGPT(*prefs));
  EXPECT_TRUE(ClaudeCredentials::IsSetUp(*prefs));
  EXPECT_EQ(prefs->GetString(prefs::kChatGPTSession).find("rt1"),
            std::string::npos);
  {
    base::AutoLock lock(lock_);
    ASSERT_EQ(token_requests_.size(), 1u);
    EXPECT_EQ(token_requests_[0]["grant_type"], "authorization_code");
    EXPECT_EQ(token_requests_[0]["client_id"], kClientId);
    EXPECT_EQ(token_requests_[0]["code"], "c0de");
    EXPECT_FALSE(token_requests_[0]["code_verifier"].empty());
    EXPECT_EQ(token_requests_[0]["resource"], chatgpt::kResource);
  }

  // The key OpenAI's requests get is the access token, on OpenAI's API.
  base::test::TestFuture<std::optional<std::string>> key;
  ClaudeCredentials::GetApiKey(profile(), key.GetCallback());
  EXPECT_EQ(key.Get(), "at1");
  EXPECT_EQ(EndpointFromPrefs(*prefs).base_url, GURL(chatgpt::kResource));

  // Close to expiring: refreshed, once for everyone waiting.
  {
    base::AutoLock lock(lock_);
    expires_in_ = 60;
  }
  ASSERT_NO_FATAL_FAILURE(SignIn());
  base::test::TestFuture<base::expected<std::string, std::string>> first;
  base::test::TestFuture<base::expected<std::string, std::string>> second;
  account()->GetAccessToken(first.GetCallback());
  account()->GetAccessToken(second.GetCallback());
  EXPECT_EQ(first.Get().value(), "at3");
  EXPECT_EQ(second.Get().value(), "at3");
  {
    base::AutoLock lock(lock_);
    // The sign-in again reused the registration; one refresh, with the
    // refresh token of the second sign-in.
    ASSERT_EQ(token_requests_.size(), 3u);
    EXPECT_EQ(token_requests_[2]["grant_type"], "refresh_token");
    EXPECT_EQ(token_requests_[2]["refresh_token"], "rt2");
    EXPECT_EQ(token_requests_[2]["client_id"], kClientId);
  }

  // Signing out forgets the tokens and revokes the latest refresh token.
  base::test::TestFuture<bool> revoked;
  account()->SignOut(revoked.GetCallback());
  EXPECT_TRUE(revoked.Get());
  EXPECT_FALSE(ChatGPTAccount::IsSignedIn(*prefs));
  EXPECT_FALSE(UsesChatGPT(*prefs));
  // The registration stays for the next sign-in.
  EXPECT_EQ(prefs->GetString(prefs::kChatGPTClientId), kClientId);
  base::AutoLock lock(lock_);
  ASSERT_EQ(revoked_.size(), 1u);
  EXPECT_EQ(revoked_[0]["token"], "rt3");
  EXPECT_EQ(revoked_[0]["token_type_hint"], "refresh_token");
}

IN_PROC_BROWSER_TEST_F(ChatGPTAccountBrowserTest, RefusedRefreshSignsOut) {
  {
    base::AutoLock lock(lock_);
    expires_in_ = 60;
  }
  ASSERT_NO_FATAL_FAILURE(SignIn());
  {
    base::AutoLock lock(lock_);
    refuse_refresh_ = true;
  }
  base::expected<std::string, std::string> token = Token();
  ASSERT_FALSE(token.has_value());
  EXPECT_NE(token.error().find("Sign in again"), std::string::npos);
  EXPECT_FALSE(ChatGPTAccount::IsSignedIn(*profile()->GetPrefs()));
  EXPECT_NE(ClaudeCredentials::MissingKeyMessage(profile()).find(
                "Sign in again"),
            std::string::npos);
}

IN_PROC_BROWSER_TEST_F(ChatGPTAccountBrowserTest, DeclinedAndStrayCallbacks) {
  base::test::TestFuture<std::optional<std::string>> done;
  GURL authorize;
  account()->SignIn(
      base::BindLambdaForTesting([&](const GURL& url) { authorize = url; }),
      done.GetCallback());
  ASSERT_TRUE(base::test::RunUntil([&] { return authorize.is_valid(); }));
  std::map<std::string, std::string> query = FormFields(authorize.query());
  // Another state: not this sign-in's, which goes on.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL(query["redirect_uri"] + "?code=x&state=other")));
  EXPECT_FALSE(done.IsReady());
  EXPECT_TRUE(account()->IsSigningIn());
  // Declined.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL(base::StrCat({query["redirect_uri"],
                                    "?error=access_denied&state=",
                                    query["state"]}))));
  ASSERT_TRUE(done.Get().has_value());
  EXPECT_NE(done.Get()->find("didn't allow"), std::string::npos);
  EXPECT_FALSE(ChatGPTAccount::IsSignedIn(*profile()->GetPrefs()));
  base::AutoLock lock(lock_);
  EXPECT_TRUE(token_requests_.empty());
}

}  // namespace
}  // namespace compass::claude
