// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/cloudflare_deployer.h"

#include <map>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/rand_util.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/strcat.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync {
namespace {

constexpr size_t kMaxReplySize = 2 * 1024 * 1024;
constexpr base::TimeDelta kRequestTimeout = base::Seconds(60);
constexpr int kSubdomainAttempts = 3;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_cloudflare_deploy", R"(
        semantics {
          sender: "Compass sync"
          description:
            "Sets up the user's own Compass sync server in the user's "
            "Cloudflare account: finds the account and its workers.dev "
            "address, uploads the sync server (a Worker shipped with "
            "Compass) with the user's sync chain as its owner, turns its "
            "address on, then checks the new server answers."
          trigger: "The user chooses to set up a free server on the sync "
                   "page and gives Compass a Cloudflare token."
          data: "The Cloudflare token the user gave, the sync server's code, "
                "and the new sync chain's public ID."
          destination: OTHER
          destination_other: "Cloudflare's API (api.cloudflare.com) and the "
                             "user's new server on workers.dev."
        }
        policy {
          cookies_allowed: NO
          setting: "Only when the user sets up a server this way."
          policy_exception_justification: "Not implemented."
        })");

std::string RandomLetters(int n) {
  std::string out;
  for (int i = 0; i < n; ++i) {
    out += static_cast<char>('a' + base::RandIntInclusive(0, 25));
  }
  return out;
}

// The API's first error message, if any.
std::string ApiMessage(const std::optional<base::Value>& json) {
  if (!json || !json->is_dict()) {
    return std::string();
  }
  const base::ListValue* errors = json->GetDict().FindList("errors");
  if (!errors || errors->empty() || !(*errors)[0].is_dict()) {
    return std::string();
  }
  const std::string* message = (*errors)[0].GetDict().FindString("message");
  return message ? *message : std::string();
}

bool Succeeded(int status, const std::optional<base::Value>& json) {
  if (status < 200 || status >= 300 || !json || !json->is_dict()) {
    return false;
  }
  std::optional<bool> success = json->GetDict().FindBool("success");
  return success.value_or(false);
}

const base::Value* ResultField(const std::optional<base::Value>& json) {
  return json && json->is_dict() ? json->GetDict().Find("result") : nullptr;
}

}  // namespace

std::optional<ServerInfo> ParseServerInfo(std::string_view json) {
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  ServerInfo info;
  if (const base::DictValue* server = value->GetDict().FindDict("server")) {
    if (const std::string* kind = server->FindString("kind")) {
      info.kind = *kind;
    }
    info.version = server->FindInt("version").value_or(0);
  }
  return info;
}

std::optional<CloudflareServerName> CloudflareServerNameOf(const GURL& url) {
  constexpr std::string_view kSuffix = ".workers.dev";
  if (!url.is_valid() || !url.SchemeIs("https")) {
    return std::nullopt;
  }
  const std::string host = base::ToLowerASCII(url.host());
  if (!host.ends_with(kSuffix)) {
    return std::nullopt;
  }
  const std::string name = host.substr(0, host.size() - kSuffix.size());
  const size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size() ||
      name.find('.', dot + 1) != std::string::npos) {
    return std::nullopt;
  }
  return CloudflareServerName{name.substr(0, dot), name.substr(dot + 1)};
}

CloudflareDeployer::CloudflareDeployer(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    std::string access_token,
    std::string script,
    GURL api_base)
    : loader_factory_(std::move(loader_factory)),
      token_(std::move(access_token)),
      script_(std::move(script)),
      api_base_(std::move(api_base)) {}

CloudflareDeployer::~CloudflareDeployer() = default;

// static
std::string CloudflareDeployer::MakeMetadata(
    const std::string& owner_chain_id,
    const std::string& migration_tag) {
  base::DictValue metadata;
  metadata.Set("main_module", "index.js");
  metadata.Set("compatibility_date", kCloudflareCompatibilityDate);
  base::ListValue bindings;
  for (const auto& [name, class_name] :
       {std::pair{"CHAIN", "Chain"}, std::pair{"PAIRING", "Pairing"},
        std::pair{"ACCESS", "Access"}, std::pair{"ROOM", "Room"}}) {
    bindings.Append(base::DictValue()
                        .Set("type", "durable_object_namespace")
                        .Set("name", name)
                        .Set("class_name", class_name));
  }
  if (owner_chain_id.empty()) {
    // An update: the owners and any other variable stay as they are.
    metadata.Set("keep_bindings",
                 base::ListValue().Append("plain_text").Append("secret_text"));
  } else {
    bindings.Append(base::DictValue()
                        .Set("type", "plain_text")
                        .Set("name", "COMPASS_SYNC_ALLOWED_CHAINS")
                        .Set("text", base::ToLowerASCII(owner_chain_id)));
  }
  metadata.Set("bindings", std::move(bindings));
  // The SQLite migration of the classes the script doesn't have yet;
  // Cloudflare refuses one a script already ran (412).
  if (migration_tag.empty()) {
    metadata.Set("migrations",
                 base::DictValue()
                     .Set("new_tag", kCloudflareMigrationTag)
                     .Set("new_sqlite_classes", base::ListValue()
                                                    .Append("Chain")
                                                    .Append("Pairing")
                                                    .Append("Access")
                                                    .Append("Room")));
  } else if (migration_tag == "v1") {
    metadata.Set("migrations",
                 base::DictValue()
                     .Set("old_tag", "v1")
                     .Set("new_tag", kCloudflareMigrationTag)
                     .Set("new_sqlite_classes", base::ListValue().Append("Room")));
  }
  return base::WriteJson(metadata).value_or("{}");
}

// static
std::string CloudflareDeployer::Describe(const Result& result) {
  switch (result.error) {
    case Error::kNone:
      return std::string();
    case Error::kUnauthorized:
      return base::StrCat(
          {"Cloudflare didn't accept the token. Check that it's complete "
           "and can edit Workers.",
           result.detail.empty() && result.step.empty() ? "" : " (",
           result.step, result.step.empty() ? "" : ": ",
           base::NumberToString(result.status),
           result.detail.empty() ? "" : " ", result.detail,
           result.detail.empty() && result.step.empty() ? "" : ")"});
    case Error::kNoAccount:
      return "The token can't use any Cloudflare account.";
    case Error::kApi:
      return result.detail.empty()
                 ? "Cloudflare refused to set up the server."
                 : base::StrCat({"Cloudflare refused: ", result.detail});
    case Error::kNetwork:
      return "Couldn't reach Cloudflare. Check your connection and try "
             "again.";
    case Error::kNotReachable:
      return "The server is set up but doesn't answer yet. Try again in a "
             "few minutes.";
    case Error::kNotFound:
      return "Your server isn't in the Cloudflare account you signed in "
             "with. Sign in with the account it was set up in.";
  }
}

void CloudflareDeployer::Update(const std::string& script_name,
                                const std::string& subdomain,
                                int version,
                                ProgressCallback progress,
                                Callback callback) {
  update_ = true;
  version_ = version;
  deployment_.script_name = script_name;
  deployment_.subdomain = subdomain;
  progress_ = std::move(progress);
  callback_ = std::move(callback);
  Progress("Finding your server on Cloudflare…");
  Call("GET", "/accounts?per_page=50", std::string(), std::string(),
       base::BindOnce(&CloudflareDeployer::OnAccounts,
                      weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::Deploy(const std::string& owner_chain_id,
                                ProgressCallback progress,
                                Callback callback) {
  owner_ = owner_chain_id;
  progress_ = std::move(progress);
  callback_ = std::move(callback);
  Progress("Checking your Cloudflare account…");
  Call("GET", "/accounts?per_page=50", std::string(), std::string(),
       base::BindOnce(&CloudflareDeployer::OnAccounts,
                      weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::Call(const std::string& method,
                              const std::string& path,
                              std::string body,
                              std::string content_type,
                              JsonCallback callback) {
  step_ = base::StrCat({method, " ", path});
  if (!deployment_.account_id.empty()) {
    base::ReplaceSubstringsAfterOffset(&step_, 0, deployment_.account_id,
                                       "{account}");
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(base::StrCat({api_base_.spec(), path}));
  request->method = method;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", token_}));
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetTimeoutDuration(kRequestTimeout);
  loader_->SetAllowHttpErrorResults(true);
  if (!content_type.empty()) {
    loader_->AttachStringForUpload(std::move(body), content_type);
  }
  loader_->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&CloudflareDeployer::OnCallDone,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      kMaxReplySize);
}

void CloudflareDeployer::OnCallDone(JsonCallback callback,
                                    std::optional<std::string> body) {
  std::unique_ptr<network::SimpleURLLoader> loader = std::move(loader_);
  const network::mojom::URLResponseHead* head = loader->ResponseInfo();
  if (!head || !head->headers) {
    Fail(Error::kNetwork);
    return;
  }
  std::optional<base::Value> json;
  if (body) {
    json = base::JSONReader::Read(*body, base::JSON_PARSE_RFC);
  }
  status_ = head->headers->response_code();
  if (status_ < 200 || status_ >= 300) {
    // Never the token: the step, the status and Cloudflare's errors.
    LOG(WARNING) << "[compass-sync] cloudflare: " << step_ << " -> " << status_
                 << " " << (body ? body->substr(0, 500) : std::string());
  }
  std::move(callback).Run(status_, std::move(json));
}

void CloudflareDeployer::OnAccounts(int status,
                                    std::optional<base::Value> json) {
  if (!Succeeded(status, json)) {
    FailFor(status, json);
    return;
  }
  const base::Value* result = ResultField(json);
  if (!result || !result->is_list() || result->GetList().empty() ||
      !result->GetList()[0].is_dict()) {
    Fail(Error::kNoAccount);
    return;
  }
  if (update_) {
    // The server's account is the one with its workers.dev subdomain.
    for (const base::Value& account : result->GetList()) {
      const std::string* account_id =
          account.is_dict() ? account.GetDict().FindString("id") : nullptr;
      if (account_id && !account_id->empty()) {
        accounts_.push_back(*account_id);
      }
    }
    CheckNextAccount();
    return;
  }
  const std::string* id = result->GetList()[0].GetDict().FindString("id");
  if (!id || id->empty()) {
    Fail(Error::kNoAccount);
    return;
  }
  deployment_.account_id = *id;
  Call("GET", base::StrCat({"/accounts/", *id, "/workers/subdomain"}),
       std::string(), std::string(),
       base::BindOnce(&CloudflareDeployer::OnSubdomain,
                      weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::CheckNextAccount() {
  if (next_account_ >= accounts_.size()) {
    Fail(Error::kNotFound);
    return;
  }
  deployment_.account_id = accounts_[next_account_++];
  Call("GET",
       base::StrCat({"/accounts/", deployment_.account_id,
                     "/workers/subdomain"}),
       std::string(), std::string(),
       base::BindOnce(&CloudflareDeployer::OnAccountSubdomain,
                      weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::OnAccountSubdomain(int status,
                                            std::optional<base::Value> json) {
  if (status == 401) {
    FailFor(status, json);
    return;
  }
  const base::Value* result = ResultField(json);
  const std::string* subdomain =
      Succeeded(status, json) && result && result->is_dict()
          ? result->GetDict().FindString("subdomain")
          : nullptr;
  if (!subdomain || *subdomain != deployment_.subdomain) {
    // Not this account (or one the sign-in can't read): the next.
    CheckNextAccount();
    return;
  }
  ListScripts();
}

void CloudflareDeployer::OnSubdomain(int status,
                                     std::optional<base::Value> json) {
  if (status == 401 || status == 403) {
    FailFor(status, json);
    return;
  }
  const base::Value* result = ResultField(json);
  const std::string* subdomain =
      Succeeded(status, json) && result && result->is_dict()
          ? result->GetDict().FindString("subdomain")
          : nullptr;
  if (subdomain && !subdomain->empty()) {
    deployment_.subdomain = *subdomain;
    ListScripts();
    return;
  }
  // The account has no workers.dev subdomain yet (404).
  CreateSubdomain(/*attempt=*/1);
}

void CloudflareDeployer::CreateSubdomain(int attempt) {
  Progress("Making your workers.dev address…");
  const std::string name = base::StrCat({"compass-", RandomLetters(8)});
  Call("PUT",
       base::StrCat({"/accounts/", deployment_.account_id,
                     "/workers/subdomain"}),
       base::WriteJson(base::DictValue().Set("subdomain", name))
           .value_or("{}"),
       "application/json",
       base::BindOnce(&CloudflareDeployer::OnSubdomainCreated,
                      weak_factory_.GetWeakPtr(), attempt));
}

void CloudflareDeployer::OnSubdomainCreated(int attempt,
                                            int status,
                                            std::optional<base::Value> json) {
  const base::Value* result = ResultField(json);
  const std::string* subdomain =
      Succeeded(status, json) && result && result->is_dict()
          ? result->GetDict().FindString("subdomain")
          : nullptr;
  if (subdomain && !subdomain->empty()) {
    deployment_.subdomain = *subdomain;
    ListScripts();
    return;
  }
  if (status != 401 && status != 403 && attempt < kSubdomainAttempts) {
    // Most likely taken: another name.
    CreateSubdomain(attempt + 1);
    return;
  }
  FailFor(status, json);
}

void CloudflareDeployer::ListScripts() {
  Call("GET",
       base::StrCat({"/accounts/", deployment_.account_id, "/workers/scripts"}),
       std::string(), std::string(),
       base::BindOnce(&CloudflareDeployer::OnScripts,
                      weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::OnScripts(int status,
                                   std::optional<base::Value> json) {
  if (!Succeeded(status, json)) {
    FailFor(status, json);
    return;
  }
  // Each script of the account, with its Durable Object migration tag.
  std::map<std::string, std::string> scripts;
  const base::Value* result = ResultField(json);
  if (result && result->is_list()) {
    for (const base::Value& script : result->GetList()) {
      if (!script.is_dict()) {
        continue;
      }
      const std::string* id = script.GetDict().FindString("id");
      const std::string* tag = script.GetDict().FindString("migration_tag");
      if (id) {
        scripts[*id] = tag ? *tag : std::string();
      }
    }
  }
  if (update_) {
    if (!scripts.contains(deployment_.script_name)) {
      // Another account with this subdomain can't exist; the script is gone.
      Fail(Error::kNotFound);
      return;
    }
  } else {
    // An account that has a "compass-sync" already (set up before, by hand
    // or for another chain) keeps it: this one gets a name of its own.
    deployment_.script_name =
        scripts.contains(kCloudflareScriptName)
            ? base::StrCat({kCloudflareScriptName, "-",
                            base::ToLowerASCII(owner_.substr(0, 6))})
            : kCloudflareScriptName;
  }
  // The migrations the script of that name (this chain's setup tried
  // before) has run already.
  const auto existing = scripts.find(deployment_.script_name);
  const std::string migration_tag =
      existing == scripts.end() ? std::string() : existing->second;

  Progress(update_ ? "Uploading the new version…"
                   : "Uploading the sync server…");
  const std::string boundary =
      base::StrCat({"compass-", RandomLetters(24)});
  std::string body;
  auto part = [&](const std::string& name, const std::string& type,
                  const std::string& content) {
    base::StrAppend(&body,
                    {"--", boundary, "\r\nContent-Disposition: form-data; name=\"",
                     name, "\"; filename=\"", name, "\"\r\nContent-Type: ", type,
                     "\r\n\r\n", content, "\r\n"});
  };
  part("metadata", "application/json", MakeMetadata(owner_, migration_tag));
  part("index.js", "application/javascript+module", script_);
  base::StrAppend(&body, {"--", boundary, "--\r\n"});
  Call("PUT",
       base::StrCat({"/accounts/", deployment_.account_id, "/workers/scripts/",
                     deployment_.script_name}),
       std::move(body),
       base::StrCat({"multipart/form-data; boundary=", boundary}),
       base::BindOnce(&CloudflareDeployer::OnUploaded,
                      weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::OnUploaded(int status,
                                    std::optional<base::Value> json) {
  if (!Succeeded(status, json)) {
    FailFor(status, json);
    return;
  }
  if (update_) {
    // Its route is on already.
    OnRouteEnabled(200, base::Value(base::DictValue().Set("success", true)));
    return;
  }
  Call("POST",
       base::StrCat({"/accounts/", deployment_.account_id, "/workers/scripts/",
                     deployment_.script_name, "/subdomain"}),
       base::WriteJson(base::DictValue().Set("enabled", true)).value_or("{}"),
       "application/json",
       base::BindOnce(&CloudflareDeployer::OnRouteEnabled,
                      weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::OnRouteEnabled(int status,
                                        std::optional<base::Value> json) {
  if (!Succeeded(status, json)) {
    FailFor(status, json);
    return;
  }
  deployment_.server_url =
      GURL(base::StrCat({"https://", deployment_.script_name, ".",
                         deployment_.subdomain, ".workers.dev/v2"}));
  Progress(update_ ? "Waiting for the new version to answer…"
                   : "Waiting for the server to come online…");
  health_deadline_ = base::TimeTicks::Now() + health_timeout_;
  CheckHealth();
}

void CloudflareDeployer::CheckHealth() {
  auto request = std::make_unique<network::ResourceRequest>();
  // An update waits for its version (/v2/compass/info); a new server for
  // its address to resolve and answer.
  request->url = update_ ? GURL(base::StrCat({deployment_.server_url.spec(),
                                              "/compass/info"}))
                         : deployment_.server_url.Resolve("/healthz");
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetTimeoutDuration(base::Seconds(15));
  loader_->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&CloudflareDeployer::OnHealth,
                     weak_factory_.GetWeakPtr()),
      4096);
}

void CloudflareDeployer::OnHealth(std::optional<std::string> body) {
  loader_.reset();
  const bool healthy =
      body && (update_ ? ParseServerInfo(*body).value_or(ServerInfo()).version >=
                             version_
                       : base::TrimWhitespaceASCII(*body, base::TRIM_ALL) == "ok");
  if (healthy) {
    Result result;
    result.deployment = deployment_;
    std::move(callback_).Run(std::move(result));
    return;
  }
  // A new workers.dev name can take a few minutes to resolve.
  if (base::TimeTicks::Now() >= health_deadline_) {
    Fail(Error::kNotReachable);
    return;
  }
  timer_.Start(FROM_HERE, health_interval_,
               base::BindOnce(&CloudflareDeployer::CheckHealth,
                              weak_factory_.GetWeakPtr()));
}

void CloudflareDeployer::FailFor(int status,
                                 const std::optional<base::Value>& json) {
  if (status == 401 || status == 403) {
    Fail(Error::kUnauthorized, json);
    return;
  }
  Fail(Error::kApi, json);
}

void CloudflareDeployer::Fail(Error error,
                              const std::optional<base::Value>& json) {
  Result result;
  result.error = error;
  result.detail = ApiMessage(json);
  result.step = step_;
  result.status = status_;
  if (error == Error::kNotReachable) {
    result.deployment = deployment_;
  }
  std::move(callback_).Run(std::move(result));
}

void CloudflareDeployer::Progress(const std::string& line) {
  if (progress_) {
    progress_.Run(line);
  }
}

}  // namespace compass::sync
