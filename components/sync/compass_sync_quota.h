// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_COMPASS_SYNC_QUOTA_H_
#define COMPASS_COMPONENTS_SYNC_COMPASS_SYNC_QUOTA_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace compass::sync {

// How full the chain is on its server: GET <server>/compass/quota
// (server/quota, docs/design/sync.md, "Server"). Chromium's engine
// retries a commit the server refuses for quota quietly, and the server
// drops visits past the history quota with SUCCESS, so this is how the
// device learns of either.
struct QuotaStatus {
  int64_t entities = 0;
  int64_t max_entities = 0;
  int64_t bytes = 0;
  int64_t max_bytes = 0;
  int64_t history = 0;
  int64_t max_history = 0;
  int history_days = 0;
  // The limits in the way now.
  bool entities_full = false;
  bool bytes_full = false;
  bool history_full = false;

  // New entities or bigger ones are refused: nothing new syncs.
  bool IsServerFull() const { return entities_full || bytes_full; }
};

// The status in a reply body, or nullopt if it isn't one. Unknown fields and
// unknown names in "full" are ignored.
std::optional<QuotaStatus> ParseQuotaStatus(std::string_view json);

// <server URL>/compass/quota (the server URL ends in /v2).
GURL MakeQuotaUrl(const GURL& server_url);

// Asks the server for the chain's QuotaStatus, signed with the chain's
// token. One request at a time.
class QuotaFetcher {
 public:
  // A fresh sync token (SyncCredentials::MakeToken), or nullopt.
  using TokenGetter = base::RepeatingCallback<std::optional<std::string>()>;
  // Nullopt: no answer, an HTTP error, or a body that isn't a status (an
  // older server answers 404).
  using Callback = base::OnceCallback<void(std::optional<QuotaStatus>)>;

  QuotaFetcher(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
               const GURL& server_url,
               TokenGetter token_getter);
  QuotaFetcher(const QuotaFetcher&) = delete;
  QuotaFetcher& operator=(const QuotaFetcher&) = delete;
  ~QuotaFetcher();

  // Fetches the status. While a fetch runs, another one is refused (false,
  // and |callback| isn't called).
  bool Fetch(Callback callback);
  bool IsFetching() const { return loader_ != nullptr; }

 private:
  void OnResponse(Callback callback, std::optional<std::string> body);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL url_;
  TokenGetter token_getter_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<QuotaFetcher> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_COMPASS_SYNC_QUOTA_H_
