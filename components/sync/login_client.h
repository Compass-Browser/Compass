// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_LOGIN_CLIENT_H_
#define COMPASS_COMPONENTS_SYNC_LOGIN_CLIENT_H_

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

// A server's optional company sign-in (docs/design/sync.md, "Company
// sign-in (OIDC)"): the server is the OIDC relying party, and a chain syncs
// only once someone signed in and the chain is bound to that user. The keys
// are untouched: sign-in only gates access.
namespace compass::sync {

// GET <server>/compass/info (no token).
struct ServerLoginInfo {
  // The server asks chains to sign in.
  bool login = false;
  // "oidc".
  std::string type;
  // What users see: "Acme SSO".
  std::string name;
};

// The signed-in user, as the identity provider named them.
struct LoginUser {
  std::string name;
  std::string email;
};

// GET <server>/compass/account.
struct AccountStatus {
  // The chain must sign in before it syncs.
  bool login_required = false;
  std::optional<LoginUser> user;
  // When the sign-in ends (the server's session); null if it doesn't.
  base::Time expires;
};

// POST <server>/compass/login: a sign-in waiting for the user.
struct LoginRequest {
  std::string id;
  // The server's page: the identity provider, then a consent page that
  // shows |user_code|.
  GURL url;
  std::string user_code;
  base::Time expires;
};

enum class LoginState {
  kPending,
  kDone,
  kDenied,
  kExpired,
  // No answer from the server (the network, or a reply that isn't one).
  kFailed,
};

struct LoginResult {
  LoginState state = LoginState::kFailed;
  std::optional<LoginUser> user;
};

// Why a request failed.
enum class LoginError {
  kNone,
  // No answer: the network, or a 5xx.
  kNetwork,
  // 404 {"error":"login_disabled"}: the server has no sign-in.
  kLoginDisabled,
  // 404 otherwise: an older server, or an unknown login.
  kNotFound,
  // 401: the token was refused.
  kUnauthorized,
  // 429: too many logins started.
  kRateLimited,
  // 503 {"error":"login_unavailable"}: the server can't reach the identity
  // provider yet.
  kLoginUnavailable,
  // Another HTTP error, or a body that isn't the answer.
  kBadReply,
};

// The reply bodies, or nullopt if they aren't one.
std::optional<ServerLoginInfo> ParseServerLoginInfo(std::string_view json);
std::optional<AccountStatus> ParseAccountStatus(std::string_view json);
std::optional<LoginRequest> ParseLoginRequest(std::string_view json);
// A poll's answer; nullopt for an unknown status.
std::optional<LoginResult> ParseLoginResult(std::string_view json);

// <server URL>/compass/<path> (the server URL ends in /v2).
GURL MakeLoginApiUrl(const GURL& server_url, std::string_view path);

// Whether a reply says the chain must sign in: HTTP 401 with
// "X-Compass-Login: required" (any gated endpoint).
bool IsLoginRequiredReply(int http_status, std::string_view login_header);
inline constexpr char kLoginHeader[] = "X-Compass-Login";
inline constexpr char kLinkedTokenHeader[] = "X-Compass-Linked-Token";

// The sign-in requests of one chain, signed with its token. Requests may
// run at the same time; destroying the client cancels them (no callback
// runs).
class LoginClient {
 public:
  // A fresh token for the chain (SyncCredentials::MakeToken), or nullopt.
  using TokenGetter = base::RepeatingCallback<std::optional<std::string>()>;
  using InfoCallback =
      base::OnceCallback<void(std::optional<ServerLoginInfo> info,
                              LoginError error)>;
  using AccountCallback =
      base::OnceCallback<void(std::optional<AccountStatus> account,
                              LoginError error)>;
  using DoneCallback = base::OnceCallback<void(LoginError error)>;
  using StartCallback =
      base::OnceCallback<void(std::optional<LoginRequest> request,
                              LoginError error)>;
  using ResultCallback = base::OnceCallback<void(LoginResult result)>;

  // A long poll lasts up to 30 s on the server; this is its limit here.
  static constexpr base::TimeDelta kPollTimeout = base::Seconds(45);
  // After a failed poll (the network), the next one waits this long.
  static constexpr base::TimeDelta kPollRetryDelay = base::Seconds(3);
  // Other requests.
  static constexpr base::TimeDelta kRequestTimeout = base::Seconds(30);

  LoginClient(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
              const GURL& server_url,
              TokenGetter token_getter);
  LoginClient(const LoginClient&) = delete;
  LoginClient& operator=(const LoginClient&) = delete;
  ~LoginClient();

  const GURL& server_url() const { return server_url_; }

  // GET /compass/info. Unsigned. An older server (404) is no login.
  void GetInfo(InfoCallback callback);
  // GET /compass/account.
  void GetAccount(AccountCallback callback);
  // DELETE /compass/account: signs the chain out (unbinds it).
  void SignOut(DoneCallback callback);
  // POST /compass/account/link: the chain whose token is |linked_token| (a
  // profile account) takes this chain's sign-in.
  void Link(const std::string& linked_token, DoneCallback callback);
  // POST /compass/login {"device_name", "return": "tab"}.
  void StartLogin(const std::string& device_name, StartCallback callback);
  // GET /compass/login/<id>?wait=1 until the answer isn't kPending: long
  // polls one after the other, retried after kPollRetryDelay when the
  // network fails, until |expires| (then kExpired). A 404 or 401 is
  // kExpired: the server forgot the login (it restarted).
  void WaitForLogin(const std::string& id,
                    base::Time expires,
                    ResultCallback callback);

 private:
  struct Reply {
    int http_status = 0;
    std::optional<std::string> body;
  };
  using ReplyCallback = base::OnceCallback<void(Reply reply)>;

  // Sends a request; |signed_request| adds the chain's token (none: a
  // kUnauthorized reply without a request).
  void Send(const std::string& method,
            const GURL& url,
            bool signed_request,
            const std::string& linked_token,
            const std::string& json_body,
            base::TimeDelta timeout,
            ReplyCallback callback);
  void OnReply(std::list<std::unique_ptr<network::SimpleURLLoader>>::iterator
                   loader,
               ReplyCallback callback,
               std::optional<std::string> body);
  void Poll(const std::string& id, base::Time expires, ResultCallback callback);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL server_url_;
  TokenGetter token_getter_;
  std::list<std::unique_ptr<network::SimpleURLLoader>> loaders_;
  base::OneShotTimer poll_retry_timer_;
  base::WeakPtrFactory<LoginClient> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_LOGIN_CLIENT_H_
