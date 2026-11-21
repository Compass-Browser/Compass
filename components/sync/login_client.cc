// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/login_client.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync {
namespace {

constexpr size_t kMaxReplySize = 16 * 1024;

// A reply that comes back sooner than this to a long poll still pending
// waits this long before the next one, so a server that doesn't hold the
// request isn't asked in a tight loop.
constexpr base::TimeDelta kMinPollGap = base::Seconds(1);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_login", R"(
        semantics {
          sender: "Compass sync"
          description:
            "Talks to the user's sync server about its optional sign-in "
            "(a company's identity provider): whether the server asks for "
            "it, who the sync chain is signed in as, starting a sign-in the "
            "user completes in a tab and waiting for it, linking the "
            "chain's profiles, and signing out when leaving sync."
          trigger: "Setting up sync with a server that asks for sign-in, the "
                   "server refusing the chain until it signs in, the user "
                   "choosing Sign in, and leaving sync."
          data: "The sync chain's signed token, and this device's name for "
                "the server's consent page. The answers hold the signed-in "
                "user's name and email."
          destination: OTHER
          destination_other: "The self-hosted sync server the user set."
        }
        policy {
          cookies_allowed: NO
          setting: "Only while the user syncs through a Compass sync server."
          policy_exception_justification: "Not implemented."
        })");

std::optional<LoginUser> ParseUser(const base::DictValue& dict) {
  const base::DictValue* user = dict.FindDict("user");
  if (!user) {
    return std::nullopt;
  }
  LoginUser result;
  if (const std::string* name = user->FindString("name")) {
    result.name = *name;
  }
  if (const std::string* email = user->FindString("email")) {
    result.email = *email;
  }
  if (result.name.empty() && result.email.empty()) {
    return std::nullopt;
  }
  return result;
}

// Unix milliseconds; 0 or absent: null.
base::Time ParseTime(const base::DictValue& dict, std::string_view key) {
  const std::optional<double> ms = dict.FindDouble(key);
  return ms && *ms > 0 ? base::Time::FromMillisecondsSinceUnixEpoch(*ms)
                       : base::Time();
}

std::optional<base::DictValue> ReadDict(const std::optional<std::string>& body) {
  if (!body) {
    return std::nullopt;
  }
  return base::JSONReader::ReadDict(*body, base::JSON_PARSE_RFC);
}

}  // namespace

std::optional<ServerLoginInfo> ParseServerLoginInfo(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  ServerLoginInfo info;
  const base::Value* login = dict->Find("login");
  if (!login || login->is_none()) {
    return info;
  }
  if (!login->is_dict()) {
    return std::nullopt;
  }
  info.login = true;
  if (const std::string* type = login->GetDict().FindString("type")) {
    info.type = *type;
  }
  if (const std::string* name = login->GetDict().FindString("name")) {
    info.name = *name;
  }
  return info;
}

std::optional<AccountStatus> ParseAccountStatus(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  const std::optional<bool> required = dict->FindBool("login_required");
  if (!required) {
    return std::nullopt;
  }
  AccountStatus status;
  status.login_required = *required;
  status.user = ParseUser(*dict);
  status.expires = ParseTime(*dict, "expires");
  return status;
}

std::optional<LoginRequest> ParseLoginRequest(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  const std::string* id = dict->FindString("id");
  const std::string* url = dict->FindString("url");
  const std::string* code = dict->FindString("user_code");
  if (!id || id->empty() || !url || !code) {
    return std::nullopt;
  }
  LoginRequest request;
  request.id = *id;
  request.url = GURL(*url);
  // Only a web page: the tab opens it.
  if (!request.url.is_valid() || !request.url.SchemeIsHTTPOrHTTPS()) {
    return std::nullopt;
  }
  request.user_code = *code;
  request.expires = ParseTime(*dict, "expires");
  return request;
}

std::optional<LoginResult> ParseLoginResult(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  const std::string* status = dict ? dict->FindString("status") : nullptr;
  if (!status) {
    return std::nullopt;
  }
  LoginResult result;
  if (*status == "pending") {
    result.state = LoginState::kPending;
  } else if (*status == "done") {
    result.state = LoginState::kDone;
    result.user = ParseUser(*dict);
  } else if (*status == "denied") {
    result.state = LoginState::kDenied;
  } else if (*status == "expired") {
    result.state = LoginState::kExpired;
  } else {
    return std::nullopt;
  }
  return result;
}

GURL MakeLoginApiUrl(const GURL& server_url, std::string_view path) {
  std::string full = server_url.GetPath();
  while (!full.empty() && full.back() == '/') {
    full.pop_back();
  }
  base::StrAppend(&full, {"/compass/", path});
  GURL::Replacements replacements;
  replacements.SetPathStr(full);
  replacements.ClearQuery();
  replacements.ClearRef();
  return server_url.ReplaceComponents(replacements);
}

bool IsLoginRequiredReply(int http_status, std::string_view login_header) {
  return http_status == 401 &&
         base::EqualsCaseInsensitiveASCII(
             base::TrimWhitespaceASCII(login_header, base::TRIM_ALL),
             "required");
}

namespace {

LoginError ErrorOf(int http_status, const std::optional<std::string>& body) {
  if (http_status >= 200 && http_status < 300) {
    return LoginError::kNone;
  }
  if (http_status == 503) {
    std::optional<base::DictValue> dict = ReadDict(body);
    const std::string* error = dict ? dict->FindString("error") : nullptr;
    if (error && *error == "login_unavailable") {
      return LoginError::kLoginUnavailable;
    }
  }
  if (http_status == 0 || http_status >= 500) {
    return LoginError::kNetwork;
  }
  if (http_status == 401) {
    return LoginError::kUnauthorized;
  }
  if (http_status == 429) {
    return LoginError::kRateLimited;
  }
  if (http_status == 404) {
    std::optional<base::DictValue> dict = ReadDict(body);
    const std::string* error = dict ? dict->FindString("error") : nullptr;
    return error && *error == "login_disabled" ? LoginError::kLoginDisabled
                                               : LoginError::kNotFound;
  }
  return LoginError::kBadReply;
}

}  // namespace

LoginClient::LoginClient(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    const GURL& server_url,
    TokenGetter token_getter)
    : loader_factory_(std::move(loader_factory)),
      server_url_(server_url),
      token_getter_(std::move(token_getter)) {}

LoginClient::~LoginClient() = default;

void LoginClient::Send(const std::string& method,
                       const GURL& url,
                       bool signed_request,
                       const std::string& linked_token,
                       const std::string& json_body,
                       base::TimeDelta timeout,
                       ReplyCallback callback) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = method;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAccept,
                             "application/json");
  if (signed_request) {
    const std::optional<std::string> token =
        token_getter_ ? token_getter_.Run() : std::nullopt;
    if (!token || token->empty()) {
      std::move(callback).Run({401, std::nullopt});
      return;
    }
    request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                               base::StrCat({"Bearer ", *token}));
  }
  if (!linked_token.empty()) {
    request->headers.SetHeader(kLinkedTokenHeader, linked_token);
  }
  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader->SetTimeoutDuration(timeout);
  // The status and body of errors are part of the answer (401, 404).
  loader->SetAllowHttpErrorResults(true);
  if (!json_body.empty()) {
    loader->AttachStringForUpload(json_body, "application/json");
  }
  network::SimpleURLLoader* raw = loader.get();
  loaders_.push_front(std::move(loader));
  raw->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&LoginClient::OnReply, weak_factory_.GetWeakPtr(),
                     loaders_.begin(), std::move(callback)),
      kMaxReplySize);
}

void LoginClient::OnReply(
    std::list<std::unique_ptr<network::SimpleURLLoader>>::iterator loader,
    ReplyCallback callback,
    std::optional<std::string> body) {
  Reply reply;
  const network::mojom::URLResponseHead* head = (*loader)->ResponseInfo();
  if (head && head->headers) {
    reply.http_status = head->headers->response_code();
  }
  if (reply.http_status != 0) {
    reply.body = std::move(body);
  }
  loaders_.erase(loader);
  std::move(callback).Run(std::move(reply));
}

void LoginClient::GetInfo(InfoCallback callback) {
  Send("GET", MakeLoginApiUrl(server_url_, "info"), /*signed_request=*/false,
       std::string(), std::string(), kRequestTimeout,
       base::BindOnce(
           [](InfoCallback callback, Reply reply) {
             const LoginError error = ErrorOf(reply.http_status, reply.body);
             if (error == LoginError::kNotFound ||
                 error == LoginError::kLoginDisabled) {
               // A server from before sign-in.
               std::move(callback).Run(ServerLoginInfo(), LoginError::kNone);
               return;
             }
             if (error != LoginError::kNone) {
               std::move(callback).Run(std::nullopt, error);
               return;
             }
             std::optional<ServerLoginInfo> info =
                 ParseServerLoginInfo(*reply.body);
             std::move(callback).Run(
                 info, info ? LoginError::kNone : LoginError::kBadReply);
           },
           std::move(callback)));
}

void LoginClient::GetAccount(AccountCallback callback) {
  Send("GET", MakeLoginApiUrl(server_url_, "account"), /*signed_request=*/true,
       std::string(), std::string(), kRequestTimeout,
       base::BindOnce(
           [](AccountCallback callback, Reply reply) {
             const LoginError error = ErrorOf(reply.http_status, reply.body);
             if (error != LoginError::kNone) {
               std::move(callback).Run(std::nullopt, error);
               return;
             }
             std::optional<AccountStatus> status =
                 ParseAccountStatus(*reply.body);
             std::move(callback).Run(
                 status, status ? LoginError::kNone : LoginError::kBadReply);
           },
           std::move(callback)));
}

void LoginClient::SignOut(DoneCallback callback) {
  Send("DELETE", MakeLoginApiUrl(server_url_, "account"),
       /*signed_request=*/true, std::string(), std::string(), kRequestTimeout,
       base::BindOnce(
           [](DoneCallback callback, Reply reply) {
             std::move(callback).Run(ErrorOf(reply.http_status, reply.body));
           },
           std::move(callback)));
}

void LoginClient::Link(const std::string& linked_token, DoneCallback callback) {
  if (linked_token.empty()) {
    std::move(callback).Run(LoginError::kBadReply);
    return;
  }
  Send("POST", MakeLoginApiUrl(server_url_, "account/link"),
       /*signed_request=*/true, linked_token, std::string(), kRequestTimeout,
       base::BindOnce(
           [](DoneCallback callback, Reply reply) {
             std::move(callback).Run(ErrorOf(reply.http_status, reply.body));
           },
           std::move(callback)));
}

void LoginClient::StartLogin(const std::string& device_name,
                             StartCallback callback) {
  base::DictValue body;
  body.Set("device_name", device_name);
  body.Set("return", "tab");
  Send("POST", MakeLoginApiUrl(server_url_, "login"), /*signed_request=*/true,
       std::string(), base::WriteJson(body).value_or("{}"), kRequestTimeout,
       base::BindOnce(
           [](StartCallback callback, Reply reply) {
             const LoginError error = ErrorOf(reply.http_status, reply.body);
             if (error != LoginError::kNone) {
               std::move(callback).Run(std::nullopt, error);
               return;
             }
             std::optional<LoginRequest> request =
                 ParseLoginRequest(*reply.body);
             std::move(callback).Run(
                 request, request ? LoginError::kNone : LoginError::kBadReply);
           },
           std::move(callback)));
}

void LoginClient::WaitForLogin(const std::string& id,
                               base::Time expires,
                               ResultCallback callback) {
  poll_retry_timer_.Stop();
  Poll(id, expires, std::move(callback));
}

void LoginClient::Poll(const std::string& id,
                       base::Time expires,
                       ResultCallback callback) {
  if (!expires.is_null() && base::Time::Now() >= expires) {
    std::move(callback).Run({LoginState::kExpired, std::nullopt});
    return;
  }
  GURL url = MakeLoginApiUrl(
      server_url_,
      base::StrCat({"login/", base::EscapePath(id)}));
  GURL::Replacements replacements;
  replacements.SetQueryStr("wait=1");
  url = url.ReplaceComponents(replacements);
  const base::TimeTicks started = base::TimeTicks::Now();
  Send("GET", url, /*signed_request=*/true, std::string(), std::string(),
       kPollTimeout,
       base::BindOnce(
           [](base::WeakPtr<LoginClient> self, std::string id,
              base::Time expires, ResultCallback callback,
              base::TimeTicks started, Reply reply) {
             if (!self) {
               return;
             }
             const base::TimeDelta took = base::TimeTicks::Now() - started;
             const LoginError error = ErrorOf(reply.http_status, reply.body);
             std::optional<LoginResult> result;
             base::TimeDelta delay;
             switch (error) {
               case LoginError::kNone:
                 result = ParseLoginResult(*reply.body);
                 if (!result) {
                   // Not an answer: try again, as for the network.
                   delay = kPollRetryDelay;
                 } else if (result->state == LoginState::kPending) {
                   delay = took < kMinPollGap ? kMinPollGap - took
                                              : base::TimeDelta();
                   result.reset();
                 }
                 break;
               case LoginError::kNetwork:
               case LoginError::kBadReply:
               case LoginError::kRateLimited:
               case LoginError::kLoginUnavailable:
                 delay = kPollRetryDelay;
                 break;
               case LoginError::kNotFound:
               case LoginError::kLoginDisabled:
               case LoginError::kUnauthorized:
                 // The server forgot the login, or no longer takes the
                 // chain's token for it.
                 result = LoginResult{LoginState::kExpired, std::nullopt};
                 break;
             }
             if (result) {
               std::move(callback).Run(std::move(*result));
               return;
             }
             VLOG(1) << "[compass-sync] login: still waiting (HTTP "
                     << reply.http_status << ")";
             self->poll_retry_timer_.Start(
                 FROM_HERE, delay,
                 base::BindOnce(&LoginClient::Poll, self, std::move(id),
                                expires, std::move(callback)));
           },
           weak_factory_.GetWeakPtr(), id, expires, std::move(callback),
           started));
}

}  // namespace compass::sync
