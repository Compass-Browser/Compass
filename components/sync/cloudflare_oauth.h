// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_CLOUDFLARE_OAUTH_H_
#define COMPASS_COMPONENTS_SYNC_CLOUDFLARE_OAUTH_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace compass::sync {

// "Sign in with Cloudflare" for setting up a server (docs/design/sync.md,
// "A free server on Cloudflare"): OAuth 2.0 authorization code with PKCE
// (S256), Compass's public client, no client secret. The access token is
// used once to deploy and never stored; the client may not ask for
// offline_access, so there is no refresh token either.
inline constexpr char kCloudflareClientId[] =
    "9a35ed0a6094c850c6b4eb2835af9d37";
// Never loaded: the browser catches the navigation to it in the sign-in
// tab and takes the code from it.
inline constexpr char kCloudflareRedirectUri[] =
    "https://compassbrowser.org/oauth/cloudflare";
inline constexpr char kCloudflareAuthorizeUrl[] =
    "https://dash.cloudflare.com/oauth2/auth";
inline constexpr char kCloudflareTokenUrl[] =
    "https://dash.cloudflare.com/oauth2/token";
// What CloudflareDeployer needs: the account, and editing Workers.
inline constexpr char kCloudflareScopes[] =
    "account-settings.read workers-scripts.admin";

// One sign-in's secrets: the PKCE verifier and the state.
struct CloudflareAuthRequest {
  std::string verifier;
  std::string state;
  // The page to open.
  GURL url;
};

// A new request: a 64-character verifier (its SHA-256 the challenge) and a
// 32-character state, both base64url.
CloudflareAuthRequest MakeCloudflareAuthRequest();

// Whether |url| is the redirect URI (whatever its query).
bool IsCloudflareRedirect(const GURL& url);

// What the redirect brought.
struct CloudflareAuthReply {
  // Set on success.
  std::optional<std::string> code;
  // The error the server gave ("access_denied"...), or "state" when the
  // state doesn't match.
  std::string error;
};
CloudflareAuthReply ParseCloudflareRedirect(const GURL& url,
                                            std::string_view expected_state);

// Trades the code for an access token at the token endpoint.
class CloudflareTokenExchange {
 public:
  // The access token, or nullopt.
  using Callback = base::OnceCallback<void(std::optional<std::string>)>;

  explicit CloudflareTokenExchange(
      scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
      GURL token_url = GURL(kCloudflareTokenUrl));
  CloudflareTokenExchange(const CloudflareTokenExchange&) = delete;
  CloudflareTokenExchange& operator=(const CloudflareTokenExchange&) = delete;
  ~CloudflareTokenExchange();

  void Exchange(const std::string& code,
                const std::string& verifier,
                Callback callback);

 private:
  void OnResponse(Callback callback, std::optional<std::string> body);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL token_url_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<CloudflareTokenExchange> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_CLOUDFLARE_OAUTH_H_
