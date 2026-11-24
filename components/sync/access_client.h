// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_ACCESS_CLIENT_H_
#define COMPASS_COMPONENTS_SYNC_ACCESS_CLIENT_H_

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace compass::sync {

// A friend on the server's access list.
struct ServerMember {
  // Their chain ID (lower-case hex).
  std::string chain;
  // The name the owner gave the invite they joined with (may be empty).
  std::string name;
  base::Time added;
  // Their profile accounts linked to it.
  int linked = 0;
};

// An invite not used yet.
struct ServerInvite {
  std::string code;
  std::string name;
  base::Time expires;
};

struct ServerPeople {
  ServerPeople();
  ServerPeople(const ServerPeople&);
  ServerPeople& operator=(const ServerPeople&);
  ~ServerPeople();

  std::vector<ServerMember> members;
  std::vector<ServerInvite> invites;
};

// An invite link, https://<server>/invite#<code> (server/cloudflare,
// "Who may use it"): the sync server URL (<server>/v2) and the code.
struct InviteLink {
  GURL server_url;
  std::string code;
};
// Nullopt unless |text| (trimmed) is such a link.
std::optional<InviteLink> ParseInviteLink(std::string_view text);

// The access list of a server that has one (the Cloudflare server,
// server/cloudflare/README.md, "Who may use it"): its owners invite
// friends with single-use codes, and friends join with them
// (docs/design/sync.md, "Inviting friends"). Every request is signed with a
// chain token.
class AccessClient {
 public:
  enum class Error {
    kNone,
    // 404 or 405: a server without an access list (the Go server).
    kUnsupported,
    // 403: only the server's owners may do that.
    kNotOwner,
    // 401: the server refused the chain.
    kUnauthorized,
    // Join: no such invite.
    kNoSuchInvite,
    // Join: the invite was used or has expired (410).
    kInviteGone,
    // 429: too many invites or friends.
    kTooMany,
    // Anything else, or no answer.
    kFailed,
  };

  using PeopleCallback =
      base::OnceCallback<void(std::optional<ServerPeople>, Error)>;
  // The invite's link.
  using InviteCallback =
      base::OnceCallback<void(std::optional<std::string>, Error)>;
  using DoneCallback = base::OnceCallback<void(Error)>;

  AccessClient(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
               const GURL& server_url);
  AccessClient(const AccessClient&) = delete;
  AccessClient& operator=(const AccessClient&) = delete;
  ~AccessClient();

  const GURL& server_url() const { return server_url_; }

  // GET /compass/access (owners).
  void List(const std::string& token, PeopleCallback callback);
  // POST /compass/access/invites {"name"} (owners).
  void CreateInvite(const std::string& token,
                    const std::string& name,
                    InviteCallback callback);
  // DELETE /compass/access/{chain or code} (owners).
  void Remove(const std::string& token,
              const std::string& target,
              DoneCallback callback);
  // POST /compass/access/join {"code"}, with the joining chain's token.
  void Join(const std::string& token,
            const std::string& code,
            DoneCallback callback);

  // A user-facing sentence for an error.
  static std::string Describe(Error error);

 private:
  using Loaders = std::list<std::unique_ptr<network::SimpleURLLoader>>;
  using ReplyCallback =
      base::OnceCallback<void(int status, std::optional<base::Value> json)>;

  void Send(const std::string& method,
            const std::string& path,
            const std::string& token,
            std::optional<std::string> json_body,
            ReplyCallback callback);
  void OnReply(Loaders::iterator loader,
               ReplyCallback callback,
               std::optional<std::string> body);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL server_url_;
  Loaders loaders_;
  base::WeakPtrFactory<AccessClient> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_ACCESS_CLIENT_H_
