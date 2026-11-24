// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_CLOUDFLARE_DEPLOYER_H_
#define COMPASS_COMPONENTS_SYNC_CLOUDFLARE_DEPLOYER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace compass::sync {

// The Worker script's settings that Compass deploys with
// (server/cloudflare/wrangler.jsonc).
inline constexpr char kCloudflareCompatibilityDate[] = "2026-11-01";
// The Durable Object migrations, as in wrangler.jsonc: v1 added Chain,
// Pairing and Access, v2 Room (rooms, docs/design/sync.md, "Rooms").
inline constexpr char kCloudflareMigrationTag[] = "v2";
inline constexpr char kCloudflareScriptName[] = "compass-sync";

// What a Compass sync server says of itself: GET <server>/compass/info's
// "server", {"kind": "cloudflare", "version": N} (server/cloudflare,
// src/version.ts). Nullopt for an answer that isn't JSON; a server without
// it (the Go server, Cloudflare servers before versions) gives an empty
// kind and version 0.
struct ServerInfo {
  std::string kind;
  int version = 0;
};
std::optional<ServerInfo> ParseServerInfo(std::string_view json);

// The Worker and workers.dev subdomain of a server URL Compass sets up,
// https://<script>.<subdomain>.workers.dev/v2; nullopt for any other URL.
struct CloudflareServerName {
  std::string script;
  std::string subdomain;
};
std::optional<CloudflareServerName> CloudflareServerNameOf(const GURL& url);

// Where the deployed server ended up.
struct CloudflareDeployment {
  std::string account_id;
  std::string script_name;
  // The account's workers.dev subdomain.
  std::string subdomain;
  // https://<script>.<subdomain>.workers.dev/v2, the sync server URL.
  GURL server_url;
};

// Deploys the Compass sync server to the user's own Cloudflare account
// (server/cloudflare/README.md, docs/design/sync.md, "A free server on
// Cloudflare"), through Cloudflare's API with an access token the user gave
// (an API token, or later an OAuth access token):
//
// 1. GET /accounts: the account (the first the token can use).
// 2. GET /accounts/{id}/workers/subdomain, or PUT one if the account has
//    none ("compass-" and 8 random letters).
// 3. GET /accounts/{id}/workers/scripts: "compass-sync" unless the account
//    has one, then "compass-sync-" and the first 6 hex of the owner chain.
// 4. PUT the script (ES module and metadata: the Durable Object bindings,
//    their SQLite migration on a new script, and
//    COMPASS_SYNC_ALLOWED_CHAINS = the owner chain).
// 5. POST its workers.dev route on.
// 6. GET <server>/healthz until "ok" (a new subdomain's DNS can take
//    minutes).
//
// Progress is reported as a line for the page; the token is used for these
// requests only, never stored.
class CloudflareDeployer {
 public:
  enum class Error {
    kNone,
    // 401 or 403 from the API: a wrong token, or one without the
    // permissions.
    kUnauthorized,
    // The token can use no account.
    kNoAccount,
    // Any other refusal from the API (its message is kept).
    kApi,
    // No answer from the API.
    kNetwork,
    // Deployed, but the server didn't answer in time.
    kNotReachable,
    // Update: the signed-in account has no such server.
    kNotFound,
  };

  struct Result {
    Error error = Error::kNone;
    // The API's own message, for the page.
    std::string detail;
    // The call that failed, its path with the account ID left out.
    std::string step;
    int status = 0;
    std::optional<CloudflareDeployment> deployment;
  };

  using ProgressCallback = base::RepeatingCallback<void(const std::string&)>;
  using Callback = base::OnceCallback<void(Result)>;

  // |api_base| is https://api.cloudflare.com/client/v4 (tests: a fake).
  // |script| is the server's bundled ES module.
  CloudflareDeployer(
      scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
      std::string access_token,
      std::string script,
      GURL api_base = GURL("https://api.cloudflare.com/client/v4"));
  CloudflareDeployer(const CloudflareDeployer&) = delete;
  CloudflareDeployer& operator=(const CloudflareDeployer&) = delete;
  ~CloudflareDeployer();

  // Deploys a server owned by |owner_chain_id| (the chain's hex public key).
  // Destroying |this| cancels it without running |callback|.
  // Updates a server Compass set up (docs/design/sync.md, "Updating a
  // server"): the account whose workers.dev subdomain is |subdomain| and
  // that has the Worker |script_name| gets the bundled server, with its
  // variables kept as they are (keep_bindings: the owners may be another
  // chain than this device's) and the migration its tag needs. Done once
  // the server's /v2/compass/info says |version| or later.
  void Update(const std::string& script_name,
              const std::string& subdomain,
              int version,
              ProgressCallback progress,
              Callback callback);

  void Deploy(const std::string& owner_chain_id,
              ProgressCallback progress,
              Callback callback);

  // How long Deploy() waits for the new server to answer.
  void set_health_timeout_for_testing(base::TimeDelta t) {
    health_timeout_ = t;
  }
  void set_health_interval_for_testing(base::TimeDelta t) {
    health_interval_ = t;
  }

  // The script's metadata part (exposed for tests).
  // |migration_tag| is the script's current one: "" for a new script (all
  // the classes, as v2), "v1" (Room added), "v2" (none). An empty
  // |owner_chain_id| (an update) keeps the script's variables instead of
  // setting the owners.
  static std::string MakeMetadata(const std::string& owner_chain_id,
                                  const std::string& migration_tag);
  // A user-facing sentence for an error.
  static std::string Describe(const Result& result);

 private:
  using JsonCallback =
      base::OnceCallback<void(int status, std::optional<base::Value> json)>;

  void Call(const std::string& method,
            const std::string& path,
            std::string body,
            std::string content_type,
            JsonCallback callback);
  void OnCallDone(JsonCallback callback, std::optional<std::string> body);

  void OnAccounts(int status, std::optional<base::Value> json);
  // Update: the accounts' subdomains, one at a time, until |subdomain|.
  void CheckNextAccount();
  void OnAccountSubdomain(int status, std::optional<base::Value> json);
  void OnSubdomain(int status, std::optional<base::Value> json);
  void CreateSubdomain(int attempt);
  void OnSubdomainCreated(int attempt,
                          int status,
                          std::optional<base::Value> json);
  void ListScripts();
  void OnScripts(int status, std::optional<base::Value> json);
  void OnUploaded(int status, std::optional<base::Value> json);
  void OnRouteEnabled(int status, std::optional<base::Value> json);
  void CheckHealth();
  void OnHealth(std::optional<std::string> body);

  // Ends with |error|; |json| (if any) gives the API's message.
  void Fail(Error error, const std::optional<base::Value>& json = std::nullopt);
  void FailFor(int status, const std::optional<base::Value>& json);
  void Progress(const std::string& line);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const std::string token_;
  const std::string script_;
  const GURL api_base_;
  base::TimeDelta health_timeout_ = base::Minutes(4);
  base::TimeDelta health_interval_ = base::Seconds(3);

  std::string owner_;
  // Update mode: what to find, and the version to wait for.
  bool update_ = false;
  int version_ = 0;
  std::vector<std::string> accounts_;
  size_t next_account_ = 0;
  // The call under way ("GET /accounts/{id}/workers/subdomain"), for
  // errors and the log.
  std::string step_;
  int status_ = 0;
  ProgressCallback progress_;
  Callback callback_;
  CloudflareDeployment deployment_;
  base::TimeTicks health_deadline_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::OneShotTimer timer_;
  base::WeakPtrFactory<CloudflareDeployer> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_CLOUDFLARE_DEPLOYER_H_
