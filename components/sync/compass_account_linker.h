// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_COMPASS_ACCOUNT_LINKER_H_
#define COMPASS_COMPONENTS_SYNC_COMPASS_ACCOUNT_LINKER_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace compass::sync {

// The header that carries the linked account's token.
inline constexpr char kLinkedTokenHeader[] = "X-Compass-Linked-Token";

// <server>/compass/account/link.
GURL MakeAccountLinkUrl(const GURL& server_url);

// Links a profile's account to the chain's own on the server
// (docs/design/sync.md, "Profiles"): POST <server>/compass/account/link with
// the home account's token, and the profile account's in
// X-Compass-Linked-Token. A private server (COMPASS_SYNC_ALLOWED_CHAINS)
// lets linked accounts in with the chain that linked them; one with
// sign-in binds them to the same user. One request at a time.
class AccountLinker {
 public:
  enum class Result {
    // 2xx, or a server without the endpoint (404, 405): nothing more to do.
    kLinked,
    // 401: the server refused the home account itself.
    kRefused,
    // Anything else, or no answer: try again later.
    kFailed,
  };
  using Callback = base::OnceCallback<void(Result)>;

  AccountLinker(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
                const GURL& server_url);
  AccountLinker(const AccountLinker&) = delete;
  AccountLinker& operator=(const AccountLinker&) = delete;
  ~AccountLinker();

  // An HTTP status (0: no answer) as a Result.
  static Result ResultOf(int status);

  // Sends the link; a request already under way is dropped (its callback
  // doesn't run). Destroying |this| drops it too.
  void Link(const std::string& home_token,
            const std::string& profile_token,
            Callback callback);
  bool busy() const { return !!loader_; }

 private:
  void OnResponse(Callback callback, std::optional<std::string> body);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL server_url_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<AccountLinker> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_COMPASS_ACCOUNT_LINKER_H_
