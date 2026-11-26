// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/chatgpt_account.h"

#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/sequence_checker.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/ai_provider.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/strings/grit/compass_strings.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/log/net_log_source.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass::claude {

namespace {

const char kUserDataKey[] = "compass_chatgpt_account";

// The user has this long to finish signing in in the browser.
constexpr base::TimeDelta kSignInTimeout = base::Minutes(10);
// The loopback server stays a moment after answering, so the page gets out.
constexpr base::TimeDelta kServerLinger = base::Seconds(3);
constexpr size_t kMaxReplySize = 256 * 1024;
constexpr base::TimeDelta kRequestTimeout = base::Seconds(30);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_chatgpt_sign_in", R"(
        semantics {
          sender: "Compass ChatGPT sign-in"
          description:
            "Signs the user in with their ChatGPT account so the OpenAI "
            "provider's requests use their ChatGPT plan: exchanges the "
            "sign-in's code for tokens, refreshes them, and revokes them on "
            "sign-out."
          trigger: "The user signs in with ChatGPT or out in Compass's AI "
                   "settings, or a request needs a fresh access token."
          data: "The OAuth code, its PKCE verifier, and the refresh token."
          destination: OTHER
          destination_other: "OpenAI's sign-in service (auth.openai.com)."
        }
        policy {
          cookies_allowed: NO
          setting: "Only after the user signs in with ChatGPT in Compass's "
                   "AI settings."
          policy_exception_justification: "Not implemented."
        })");

int ResponseCode(const network::SimpleURLLoader& loader) {
  const network::mojom::URLResponseHead* head = loader.ResponseInfo();
  return head && head->headers ? head->headers->response_code() : 0;
}

}  // namespace

// Listens on 127.0.0.1 (the preferred port, else any) for the browser's
// redirect, on the IO thread, and hands each request for the callback path
// to the UI thread, which answers it once the sign-in is done.
class ChatGPTAccount::CallbackServer {
 public:
  // The port it listens on; 0 if it couldn't.
  using StartedCallback = base::OnceCallback<void(int port)>;
  using RequestCallback =
      base::RepeatingCallback<void(int connection_id, const GURL& url)>;

  CallbackServer(StartedCallback started, RequestCallback request)
      : core_(content::GetIOThreadTaskRunner({}),
              base::BindPostTaskToCurrentDefault(std::move(started)),
              base::BindPostTaskToCurrentDefault(std::move(request))) {}

  void Respond(int connection_id, std::string html) {
    core_.AsyncCall(&Core::Respond).WithArgs(connection_id, std::move(html));
  }

 private:
  class Core : public net::HttpServer::Delegate {
   public:
    Core(StartedCallback started, RequestCallback request)
        : request_(std::move(request)) {
      for (int port : {chatgpt::kPreferredPort, 0}) {
        auto socket = std::make_unique<net::TCPServerSocket>(
            nullptr, net::NetLogSource());
        if (socket->Listen(net::IPEndPoint(net::IPAddress::IPv4Localhost(),
                                           port),
                           /*backlog=*/4, /*ipv6_only=*/std::nullopt) !=
            net::OK) {
          continue;
        }
        server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
        net::IPEndPoint address;
        if (server_->GetLocalAddress(&address) != net::OK) {
          server_.reset();
          continue;
        }
        std::move(started).Run(address.port());
        return;
      }
      std::move(started).Run(0);
    }

    void Respond(int connection_id, const std::string& html) {
      if (server_) {
        server_->Send200(connection_id, html, "text/html; charset=utf-8",
                         kTrafficAnnotation);
      }
    }

    // net::HttpServer::Delegate:
    void OnConnect(int connection_id) override {}
    void OnHttpRequest(int connection_id,
                       const net::HttpServerRequestInfo& info) override {
      // |path| is the request target, query included.
      const GURL url("http://127.0.0.1" + info.path);
      if (!url.is_valid() || url.path() != chatgpt::kCallbackPath) {
        // The browser's favicon request, or anything else.
        server_->Send404(connection_id, kTrafficAnnotation);
        return;
      }
      request_.Run(connection_id, url);
    }
    void OnWebSocketRequest(int connection_id,
                            const net::HttpServerRequestInfo& info) override {
      server_->Close(connection_id);
    }
    void OnWebSocketMessage(int connection_id, std::string data) override {}
    void OnClose(int connection_id) override {}

   private:
    RequestCallback request_;
    std::unique_ptr<net::HttpServer> server_;
  };

  base::SequenceBound<Core> core_;
};

struct ChatGPTAccount::PendingSignIn {
  std::string state;
  std::string nonce;
  std::string verifier;
  // The registration it started with (empty: a first sign-in), and the one
  // the callback confirmed.
  std::string saved_client_id;
  std::string client_id;
  std::string redirect_uri;
  SignInCallback done;
  std::unique_ptr<CallbackServer> server;
  // The browser's request, waiting for the page that ends the sign-in.
  std::optional<int> connection_id;
  std::unique_ptr<network::SimpleURLLoader> loader;
  base::OneShotTimer timeout;
};

// static
ChatGPTAccount* ChatGPTAccount::ForProfile(Profile* profile) {
  auto* account =
      static_cast<ChatGPTAccount*>(profile->GetUserData(kUserDataKey));
  if (!account) {
    auto owned = std::make_unique<ChatGPTAccount>(profile);
    account = owned.get();
    profile->SetUserData(kUserDataKey, std::move(owned));
  }
  return account;
}

// static
bool ChatGPTAccount::IsSignedIn(const PrefService& prefs) {
  return !prefs.GetString(prefs::kChatGPTSession).empty();
}

// static
std::string ChatGPTAccount::AccountEmail(const PrefService& prefs) {
  return IsSignedIn(prefs) ? prefs.GetString(prefs::kChatGPTEmail)
                           : std::string();
}

ChatGPTAccount::ChatGPTAccount(Profile* profile)
    : profile_(profile),
      authorize_url_(chatgpt::kAuthorizeUrl),
      token_url_(chatgpt::kTokenUrl),
      discovery_url_(chatgpt::kDiscoveryUrl) {}

ChatGPTAccount::~ChatGPTAccount() = default;

void ChatGPTAccount::SetUrlsForTesting(const GURL& authorize_url,
                                       const GURL& token_url,
                                       const GURL& discovery_url) {
  authorize_url_ = authorize_url;
  token_url_ = token_url;
  discovery_url_ = discovery_url;
}

void ChatGPTAccount::SetLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  loader_factory_for_testing_ = std::move(factory);
}

scoped_refptr<network::SharedURLLoaderFactory>
ChatGPTAccount::LoaderFactory() {
  if (loader_factory_for_testing_) {
    return loader_factory_for_testing_;
  }
  return profile_->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess();
}

std::unique_ptr<network::SimpleURLLoader> ChatGPTAccount::MakePost(
    const GURL& url,
    const std::string& body) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = body.empty() ? "GET" : "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAccept,
                             "application/json");
  auto loader =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  if (!body.empty()) {
    loader->AttachStringForUpload(body, "application/x-www-form-urlencoded");
  }
  loader->SetAllowHttpErrorResults(true);
  loader->SetTimeoutDuration(kRequestTimeout);
  return loader;
}

// Sign-in -------------------------------------------------------------------

void ChatGPTAccount::SignIn(OpenUrlCallback open_url, SignInCallback done) {
  CancelSignIn();
  PrefService* prefs = profile_->GetPrefs();
  // Made once, before the first sign-in, and kept.
  if (prefs->GetString(prefs::kChatGPTHostId).empty()) {
    prefs->SetString(prefs::kChatGPTHostId, chatgpt::NewHostId());
  }
  pending_ = std::make_unique<PendingSignIn>();
  pending_->state = chatgpt::RandomToken();
  pending_->nonce = chatgpt::RandomToken();
  pending_->verifier = chatgpt::RandomToken();
  pending_->saved_client_id = prefs->GetString(prefs::kChatGPTClientId);
  pending_->done = std::move(done);
  pending_->server = std::make_unique<CallbackServer>(
      base::BindOnce(&ChatGPTAccount::OnServerStarted,
                     weak_factory_.GetWeakPtr(), std::move(open_url)),
      base::BindRepeating(&ChatGPTAccount::OnCallbackRequest,
                          weak_factory_.GetWeakPtr()));
  pending_->timeout.Start(
      FROM_HERE, kSignInTimeout,
      base::BindOnce(&ChatGPTAccount::FinishSignIn, base::Unretained(this),
                     std::optional<std::string>(l10n_util::GetStringUTF8(
                         IDS_COMPASS_CHATGPT_SIGN_IN_TIMED_OUT))));
}

void ChatGPTAccount::CancelSignIn() {
  if (pending_) {
    FinishSignIn(
        l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_SIGN_IN_CANCELED));
  }
}

void ChatGPTAccount::OnServerStarted(OpenUrlCallback open_url, int port) {
  if (!pending_ || !pending_->redirect_uri.empty()) {
    return;
  }
  if (!port) {
    FinishSignIn(l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_NO_LISTENER));
    return;
  }
  pending_->redirect_uri = chatgpt::RedirectUri(port);
  const PrefService& prefs = *profile_->GetPrefs();
  chatgpt::AuthorizeParams params;
  params.client_id = pending_->saved_client_id;
  params.redirect_uri = pending_->redirect_uri;
  params.state = pending_->state;
  params.nonce = pending_->nonce;
  params.code_challenge = chatgpt::CodeChallenge(pending_->verifier);
  params.host_id = prefs.GetString(prefs::kChatGPTHostId);
  // Signing in again while signed in: the same account, no selector.
  if (session_ && !params.client_id.empty()) {
    params.id_token_hint = session_->id_token;
  }
  params.login_hint = prefs.GetString(prefs::kChatGPTEmail);
  GURL url = chatgpt::AuthorizeUrl(params);
  if (authorize_url_ != GURL(chatgpt::kAuthorizeUrl)) {
    // Tests: the same query, at their server.
    GURL::Replacements replacements;
    replacements.SetQueryStr(url.query());
    url = authorize_url_.ReplaceComponents(replacements);
  }
  std::move(open_url).Run(url);
}

void ChatGPTAccount::OnCallbackRequest(int connection_id, const GURL& url) {
  const chatgpt::CallbackResult callback = chatgpt::ParseCallback(url);
  if (!pending_ || pending_->connection_id ||
      callback.state != pending_->state) {
    // Not this sign-in's (an old tab, or another page reaching the port).
    if (pending_) {
      pending_->server->Respond(
          connection_id,
          chatgpt::CallbackPage(
              false, l10n_util::GetStringUTF8(
                         IDS_COMPASS_CHATGPT_CALLBACK_WRONG_SIGN_IN)));
    }
    return;
  }
  pending_->connection_id = connection_id;
  if (!callback.error.empty()) {
    FinishSignIn(callback.error == "access_denied"
                     ? l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_DENIED)
                     : (callback.error_description.empty()
                            ? l10n_util::GetStringFUTF8(
                                  IDS_COMPASS_CHATGPT_REFUSED,
                                  base::UTF8ToUTF16(callback.error))
                            : callback.error_description));
    return;
  }
  if (callback.code.empty()) {
    FinishSignIn(l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_NO_CODE));
    return;
  }
  base::expected<std::string, std::string> client =
      chatgpt::ClientForCallback(pending_->saved_client_id, callback);
  if (!client.has_value()) {
    FinishSignIn(client.error());
    return;
  }
  pending_->client_id = *client;
  if (pending_->saved_client_id.empty()) {
    // Registered: kept from now on, whatever happens next.
    profile_->GetPrefs()->SetString(prefs::kChatGPTClientId, *client);
  }
  pending_->loader = MakePost(
      token_url_,
      chatgpt::CodeExchangeBody(*client, callback.code, pending_->verifier,
                                pending_->redirect_uri));
  pending_->loader->DownloadToString(
      LoaderFactory().get(),
      base::BindOnce(&ChatGPTAccount::OnCodeExchanged,
                     weak_factory_.GetWeakPtr()),
      kMaxReplySize);
}

void ChatGPTAccount::OnCodeExchanged(std::optional<std::string> body) {
  if (!pending_ || !pending_->loader) {
    return;
  }
  const int status = ResponseCode(*pending_->loader);
  pending_->loader.reset();
  base::expected<chatgpt::TokenReply, chatgpt::TokenError> reply =
      chatgpt::ParseTokenReply(body ? status : 0, body.value_or(""));
  if (!reply.has_value()) {
    if (reply.error().code == "invalid_client") {
      // The registration is gone: the next sign-in registers anew.
      profile_->GetPrefs()->ClearPref(prefs::kChatGPTClientId);
    }
    FinishSignIn(
        reply.error().code == "invalid_grant"
            ? l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_CODE_REFUSED)
            : reply.error().message);
    return;
  }
  const base::Time now = base::Time::Now();
  base::expected<chatgpt::IdTokenClaims, std::string> claims =
      chatgpt::CheckIdToken(reply->id_token, pending_->client_id,
                            pending_->nonce, now);
  if (!claims.has_value()) {
    FinishSignIn(claims.error());
    return;
  }
  if (!chatgpt::HasPlanScope(reply->scope)) {
    FinishSignIn(l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_NO_PLAN_SCOPE));
    return;
  }
  chatgpt::Session session;
  session.client_id = pending_->client_id;
  session.Apply(*reply, now);
  session.subject = claims->subject;
  session.email = claims->email;
  if (session.refresh_token.empty()) {
    FinishSignIn(
        l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_NO_REFRESH_TOKEN));
    return;
  }
  StoreSession(session,
               base::BindOnce(&ChatGPTAccount::OnSignInStored,
                              weak_factory_.GetWeakPtr(), session));
}

void ChatGPTAccount::OnSignInStored(chatgpt::Session session, bool stored) {
  if (!pending_) {
    return;
  }
  if (!stored) {
    FinishSignIn(l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_NOT_STORED));
    return;
  }
  ++generation_;
  profile_->GetPrefs()->SetString(prefs::kChatGPTEmail, session.email);
  session_ = std::move(session);
  ended_reason_.clear();
  last_token_error_.clear();
  FinishSignIn(std::nullopt);
}

void ChatGPTAccount::FinishSignIn(std::optional<std::string> error) {
  if (!pending_) {
    return;
  }
  std::unique_ptr<PendingSignIn> pending = std::move(pending_);
  if (pending->connection_id) {
    pending->server->Respond(
        *pending->connection_id,
        chatgpt::CallbackPage(
            !error, error ? *error
                          : l10n_util::GetStringUTF8(
                                IDS_COMPASS_CHATGPT_CALLBACK_SIGNED_IN)));
  }
  // The server goes a moment later, once the page is out.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::DoNothingWithBoundArgs(std::move(pending->server)),
      kServerLinger);
  if (pending->done) {
    std::move(pending->done).Run(std::move(error));
  }
}

// The tokens ----------------------------------------------------------------

void ChatGPTAccount::LoadSession(base::OnceCallback<void(bool)> callback) {
  if (session_) {
    std::move(callback).Run(true);
    return;
  }
  if (!IsSignedIn(*profile_->GetPrefs())) {
    std::move(callback).Run(false);
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(
      base::BindOnce(&ChatGPTAccount::OnEncryptorForLoad,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ChatGPTAccount::OnEncryptorForLoad(
    base::OnceCallback<void(bool)> callback,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (session_) {
    std::move(callback).Run(true);
    return;
  }
  std::string ciphertext;
  std::string json;
  if (encryptor &&
      base::Base64Decode(profile_->GetPrefs()->GetString(
                             prefs::kChatGPTSession),
                         &ciphertext) &&
      !ciphertext.empty() && encryptor->DecryptString(ciphertext, &json)) {
    if (std::optional<base::DictValue> dict =
            base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC)) {
      session_ = chatgpt::Session::FromValue(*dict);
    }
  }
  std::move(callback).Run(session_.has_value());
}

void ChatGPTAccount::StoreSession(const chatgpt::Session& session,
                                  base::OnceCallback<void(bool)> callback) {
  std::optional<std::string> json = base::WriteJson(session.ToValue());
  if (!json) {
    std::move(callback).Run(false);
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      [](base::WeakPtr<ChatGPTAccount> self, std::string json,
         base::OnceCallback<void(bool)> callback,
         scoped_refptr<os_crypt_async::Encryptor> encryptor) {
        std::string ciphertext;
        if (!self || !encryptor ||
            !encryptor->EncryptString(json, &ciphertext)) {
          std::move(callback).Run(false);
          return;
        }
        self->profile_->GetPrefs()->SetString(prefs::kChatGPTSession,
                                              base::Base64Encode(ciphertext));
        std::move(callback).Run(true);
      },
      weak_factory_.GetWeakPtr(), std::move(*json), std::move(callback)));
}

void ChatGPTAccount::ClearSession() {
  ++generation_;
  session_.reset();
  PrefService* prefs = profile_->GetPrefs();
  prefs->ClearPref(prefs::kChatGPTSession);
  prefs->ClearPref(prefs::kChatGPTEmail);
}

void ChatGPTAccount::GetAccessToken(TokenCallback callback) {
  if (!IsSignedIn(*profile_->GetPrefs())) {
    last_token_error_ =
        ended_reason_.empty()
            ? l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_SIGN_IN_IN_SETTINGS)
            : ended_reason_;
    std::move(callback).Run(base::unexpected(last_token_error_));
    return;
  }
  waiting_.push_back(std::move(callback));
  if (getting_token_) {
    return;
  }
  getting_token_ = true;
  LoadSession(base::BindOnce(&ChatGPTAccount::OnLoadedForToken,
                             weak_factory_.GetWeakPtr()));
}

void ChatGPTAccount::OnLoadedForToken(bool loaded) {
  if (!loaded) {
    // Can't be decrypted any more (the OS's storage was reset).
    ResolveWaiting(base::unexpected(
        l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_SESSION_UNREADABLE)));
    return;
  }
  if (session_->IsFresh(base::Time::Now())) {
    ResolveWaiting(base::ok(session_->access_token));
    return;
  }
  refresh_loader_ = MakePost(
      token_url_,
      chatgpt::RefreshBody(session_->client_id, session_->refresh_token));
  refresh_loader_->DownloadToString(
      LoaderFactory().get(),
      base::BindOnce(&ChatGPTAccount::OnRefreshed, weak_factory_.GetWeakPtr(),
                     generation_),
      kMaxReplySize);
}

void ChatGPTAccount::OnRefreshed(int generation,
                                 std::optional<std::string> body) {
  const int status = body ? ResponseCode(*refresh_loader_) : 0;
  refresh_loader_.reset();
  if (generation != generation_ || !session_) {
    // Signed out, or in again, meanwhile: theirs stands.
    if (session_) {
      ResolveWaiting(base::ok(session_->access_token));
    } else {
      ResolveWaiting(base::unexpected(
          l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_SIGN_IN_IN_SETTINGS)));
    }
    return;
  }
  base::expected<chatgpt::TokenReply, chatgpt::TokenError> reply =
      chatgpt::ParseTokenReply(status, body.value_or(""));
  if (!reply.has_value()) {
    if (reply.error().needs_sign_in) {
      // Over: the registration stays for signing in again.
      ClearSession();
      ended_reason_ =
          l10n_util::GetStringUTF8(IDS_COMPASS_CHATGPT_SESSION_ENDED);
      ResolveWaiting(base::unexpected(ended_reason_));
      return;
    }
    // A network or server failure keeps the tokens for the next try.
    ResolveWaiting(base::unexpected(reply.error().message));
    return;
  }
  session_->Apply(*reply, base::Time::Now());
  // Saved before anything else refreshes: the old refresh token is spent.
  StoreSession(*session_, base::DoNothing());
  ResolveWaiting(base::ok(session_->access_token));
}

void ChatGPTAccount::ResolveWaiting(
    base::expected<std::string, std::string> result) {
  getting_token_ = false;
  last_token_error_ = result.has_value() ? std::string() : result.error();
  std::vector<TokenCallback> waiting = std::move(waiting_);
  base::WeakPtr<ChatGPTAccount> self = weak_factory_.GetWeakPtr();
  for (TokenCallback& callback : waiting) {
    std::move(callback).Run(result);
    if (!self) {
      return;
    }
  }
}

// Sign-out ------------------------------------------------------------------

void ChatGPTAccount::SignOut(base::OnceCallback<void(bool revoked)> done) {
  CancelSignIn();
  ended_reason_.clear();
  LoadSession(base::BindOnce(&ChatGPTAccount::OnLoadedForSignOut,
                             weak_factory_.GetWeakPtr(), std::move(done)));
}

void ChatGPTAccount::OnLoadedForSignOut(base::OnceCallback<void(bool)> done,
                                        bool loaded) {
  std::string client_id;
  std::string refresh_token;
  if (loaded && session_) {
    client_id = session_->client_id;
    refresh_token = session_->refresh_token;
  }
  // Forgotten here whatever OpenAI says.
  ClearSession();
  if (refresh_token.empty()) {
    std::move(done).Run(false);
    return;
  }
  Revoke(std::move(client_id), std::move(refresh_token), std::move(done));
}

void ChatGPTAccount::Revoke(std::string client_id,
                            std::string refresh_token,
                            base::OnceCallback<void(bool)> done) {
  revoke_loader_ = MakePost(discovery_url_, std::string());
  revoke_loader_->DownloadToString(
      LoaderFactory().get(),
      base::BindOnce(&ChatGPTAccount::OnDiscovery, weak_factory_.GetWeakPtr(),
                     std::move(client_id), std::move(refresh_token),
                     std::move(done)),
      kMaxReplySize);
}

void ChatGPTAccount::OnDiscovery(std::string client_id,
                                 std::string refresh_token,
                                 base::OnceCallback<void(bool)> done,
                                 std::optional<std::string> body) {
  const int status = body ? ResponseCode(*revoke_loader_) : 0;
  revoke_loader_.reset();
  std::optional<GURL> endpoint =
      status == 200 ? chatgpt::RevocationEndpoint(*body) : std::nullopt;
  if (endpoint && discovery_url_ != GURL(chatgpt::kDiscoveryUrl)) {
    // Tests: at their server.
    GURL::Replacements replacements;
    replacements.SetPathStr(endpoint->path());
    endpoint = discovery_url_.ReplaceComponents(replacements);
  }
  if (!endpoint) {
    std::move(done).Run(false);
    return;
  }
  revoke_loader_ =
      MakePost(*endpoint, chatgpt::RevokeBody(client_id, refresh_token));
  revoke_loader_->DownloadToString(
      LoaderFactory().get(),
      base::BindOnce(&ChatGPTAccount::OnRevoked, weak_factory_.GetWeakPtr(),
                     std::move(done)),
      kMaxReplySize);
}

void ChatGPTAccount::OnRevoked(base::OnceCallback<void(bool)> done,
                               std::optional<std::string> body) {
  // An empty 200, also for a token already invalid.
  const bool revoked = body && ResponseCode(*revoke_loader_) == 200;
  revoke_loader_.reset();
  std::move(done).Run(revoked);
}

}  // namespace compass::claude
