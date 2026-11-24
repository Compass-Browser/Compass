// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/access_client.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync {
namespace {

constexpr size_t kMaxReplySize = 256 * 1024;
constexpr base::TimeDelta kRequestTimeout = base::Seconds(30);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_access", R"(
        semantics {
          sender: "Compass sync"
          description:
            "Manages who may use the user's own sync server: lists the "
            "friends the user invited, makes or revokes an invite, removes "
            "a friend; or, for someone invited, joins the server with the "
            "invite's code."
          trigger: "The user opens the people on their server, invites or "
                   "removes someone on the sync page, or joins a server with "
                   "an invite link."
          data: "The chain's signed token; an invite's code and the name "
                "the user gave it; a friend's chain ID."
          destination: OTHER
          destination_other: "The sync server the user set or was invited "
                             "to."
        }
        policy {
          cookies_allowed: NO
          setting: "Only while the user syncs through a Compass sync server, "
                   "or joins one."
          policy_exception_justification: "Not implemented."
        })");

AccessClient::Error ErrorOf(int status, const std::optional<base::Value>& json) {
  using Error = AccessClient::Error;
  if (status >= 200 && status < 300) {
    return Error::kNone;
  }
  const std::string* code =
      json && json->is_dict() ? json->GetDict().FindString("error") : nullptr;
  switch (status) {
    case 401:
      return Error::kUnauthorized;
    case 403:
      return Error::kNotOwner;
    case 404:
      return code && *code == "no_such_invite" ? Error::kNoSuchInvite
                                               : Error::kUnsupported;
    case 405:
      return Error::kUnsupported;
    case 410:
      return Error::kInviteGone;
    case 429:
      return Error::kTooMany;
    default:
      return Error::kFailed;
  }
}

bool IsInviteCode(std::string_view code) {
  if (code.size() < 10 || code.size() > 64) {
    return false;
  }
  for (char c : code) {
    if (!base::IsAsciiLower(c) && !(c >= '2' && c <= '7')) {
      return false;
    }
  }
  return true;
}

}  // namespace

ServerPeople::ServerPeople() = default;
ServerPeople::ServerPeople(const ServerPeople&) = default;
ServerPeople& ServerPeople::operator=(const ServerPeople&) = default;
ServerPeople::~ServerPeople() = default;

std::optional<InviteLink> ParseInviteLink(std::string_view text) {
  const GURL url(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !url.has_host() ||
      !url.has_ref()) {
    return std::nullopt;
  }
  std::string path = url.GetPath();
  constexpr std::string_view kInvite = "/invite";
  if (!path.ends_with(kInvite)) {
    return std::nullopt;
  }
  const std::string code = base::ToLowerASCII(url.GetRef());
  if (!IsInviteCode(code)) {
    return std::nullopt;
  }
  path.resize(path.size() - kInvite.size());
  GURL::Replacements replacements;
  const std::string server_path = base::StrCat({path, "/v2"});
  replacements.SetPathStr(server_path);
  replacements.ClearQuery();
  replacements.ClearRef();
  return InviteLink{url.ReplaceComponents(replacements), code};
}

AccessClient::AccessClient(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    const GURL& server_url)
    : loader_factory_(std::move(loader_factory)), server_url_(server_url) {}

AccessClient::~AccessClient() = default;

// static
std::string AccessClient::Describe(Error error) {
  switch (error) {
    case Error::kNone:
      return std::string();
    case Error::kUnsupported:
      return "This server doesn't take invites.";
    case Error::kNotOwner:
      return "Only the server's owner can do that.";
    case Error::kUnauthorized:
      return "The server refused this device.";
    case Error::kNoSuchInvite:
      return "This invite doesn't exist. Check the link.";
    case Error::kInviteGone:
      return "This invite was used or has expired. Ask for a new one.";
    case Error::kTooMany:
      return "The server has too many invites or people. Remove some "
             "first.";
    case Error::kFailed:
      return "Couldn't reach the sync server. Try again.";
  }
}

void AccessClient::List(const std::string& token, PeopleCallback callback) {
  Send("GET", "/compass/access", token, std::nullopt,
       base::BindOnce(
           [](PeopleCallback callback, int status,
              std::optional<base::Value> json) {
             const Error error = ErrorOf(status, json);
             if (error != Error::kNone || !json || !json->is_dict()) {
               std::move(callback).Run(
                   std::nullopt, error == Error::kNone ? Error::kFailed : error);
               return;
             }
             ServerPeople people;
             const base::DictValue& dict = json->GetDict();
             if (const base::ListValue* members = dict.FindList("members")) {
               for (const base::Value& m : *members) {
                 if (!m.is_dict()) {
                   continue;
                 }
                 const std::string* chain = m.GetDict().FindString("chain");
                 if (!chain) {
                   continue;
                 }
                 const std::string* name = m.GetDict().FindString("name");
                 people.members.push_back(
                     {*chain, name ? *name : std::string(),
                      base::Time::FromSecondsSinceUnixEpoch(
                          m.GetDict().FindDouble("added_at").value_or(0)),
                      m.GetDict().FindInt("linked").value_or(0)});
               }
             }
             if (const base::ListValue* invites = dict.FindList("invites")) {
               const base::Time now = base::Time::Now();
               for (const base::Value& i : *invites) {
                 if (!i.is_dict()) {
                   continue;
                 }
                 const std::string* code = i.GetDict().FindString("code");
                 if (!code) {
                   continue;
                 }
                 const std::string* name = i.GetDict().FindString("name");
                 people.invites.push_back(
                     {*code, name ? *name : std::string(),
                      now + base::Seconds(
                                i.GetDict().FindDouble("expires_in").value_or(0))});
               }
             }
             std::move(callback).Run(std::move(people), Error::kNone);
           },
           std::move(callback)));
}

void AccessClient::CreateInvite(const std::string& token,
                                const std::string& name,
                                InviteCallback callback) {
  Send("POST", "/compass/access/invites", token,
       base::WriteJson(base::DictValue().Set("name", name)),
       base::BindOnce(
           [](InviteCallback callback, int status,
              std::optional<base::Value> json) {
             const Error error = ErrorOf(status, json);
             const std::string* link =
                 json && json->is_dict() ? json->GetDict().FindString("link")
                                         : nullptr;
             if (error != Error::kNone || !link) {
               std::move(callback).Run(
                   std::nullopt, error == Error::kNone ? Error::kFailed : error);
               return;
             }
             std::move(callback).Run(*link, Error::kNone);
           },
           std::move(callback)));
}

void AccessClient::Remove(const std::string& token,
                          const std::string& target,
                          DoneCallback callback) {
  Send("DELETE",
       base::StrCat({"/compass/access/",
                     base::EscapeAllExceptUnreserved(target)}),
       token, std::nullopt,
       base::BindOnce(
           [](DoneCallback callback, int status,
              std::optional<base::Value> json) {
             std::move(callback).Run(ErrorOf(status, json));
           },
           std::move(callback)));
}

void AccessClient::Join(const std::string& token,
                        const std::string& code,
                        DoneCallback callback) {
  Send("POST", "/compass/access/join", token,
       base::WriteJson(base::DictValue().Set("code", code)),
       base::BindOnce(
           [](DoneCallback callback, int status,
              std::optional<base::Value> json) {
             std::move(callback).Run(ErrorOf(status, json));
           },
           std::move(callback)));
}

void AccessClient::Send(const std::string& method,
                        const std::string& path,
                        const std::string& token,
                        std::optional<std::string> json_body,
                        ReplyCallback callback) {
  std::string base_path = server_url_.GetPath();
  while (!base_path.empty() && base_path.back() == '/') {
    base_path.pop_back();
  }
  GURL::Replacements replacements;
  const std::string full_path = base::StrCat({base_path, path});
  replacements.SetPathStr(full_path);
  replacements.ClearQuery();
  replacements.ClearRef();

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = server_url_.ReplaceComponents(replacements);
  request->method = method;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", token}));
  loaders_.push_front(
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation));
  Loaders::iterator loader = loaders_.begin();
  (*loader)->SetTimeoutDuration(kRequestTimeout);
  (*loader)->SetAllowHttpErrorResults(true);
  if (json_body) {
    (*loader)->AttachStringForUpload(*json_body, "application/json");
  }
  (*loader)->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&AccessClient::OnReply, weak_factory_.GetWeakPtr(),
                     loader, std::move(callback)),
      kMaxReplySize);
}

void AccessClient::OnReply(Loaders::iterator loader,
                           ReplyCallback callback,
                           std::optional<std::string> body) {
  const network::mojom::URLResponseHead* head = (*loader)->ResponseInfo();
  const int status =
      head && head->headers ? head->headers->response_code() : 0;
  loaders_.erase(loader);
  std::optional<base::Value> json;
  if (body) {
    json = base::JSONReader::Read(*body, base::JSON_PARSE_RFC);
  }
  std::move(callback).Run(status, std::move(json));
}

}  // namespace compass::sync
