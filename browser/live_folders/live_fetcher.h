// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_LIVE_FOLDERS_LIVE_FETCHER_H_
#define COMPASS_BROWSER_LIVE_FOLDERS_LIVE_FETCHER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace compass {

// Why a refresh failed: a code from the contract (docs/design/live-folders.md,
// "Runtime") and a message for the user.
struct LiveError {
  std::string code;
  std::string message;
};

// What a plugin asks compass.fetch() for.
struct LiveFetchRequest {
  LiveFetchRequest();
  LiveFetchRequest(const LiveFetchRequest&);
  LiveFetchRequest& operator=(const LiveFetchRequest&);
  ~LiveFetchRequest();

  std::string url;
  std::string method = "GET";
  std::vector<std::pair<std::string, std::string>> headers;
  std::optional<std::string> body;
  // credentials: "include".
  bool include_credentials = false;
};

struct LiveFetchResponse {
  LiveFetchResponse();
  LiveFetchResponse(LiveFetchResponse&&);
  LiveFetchResponse& operator=(LiveFetchResponse&&);
  ~LiveFetchResponse();

  int status = 0;
  // After redirects.
  GURL url;
  // Lower-case names; repeated headers joined with ", ".
  std::map<std::string, std::string> headers;
  // Decoded with the Content-Type's charset, UTF-8 otherwise.
  std::string body;
};

// One request a refresh made, for tests and the developer view.
struct LiveFetchRecord {
  std::string method;
  GURL url;
  bool cookies = false;
};

// compass.fetch() for one refresh, in the browser process: every rule the
// plugin's context can't be trusted with. Hosts are checked on every
// redirect hop, the host owns the sensitive headers, responses are capped,
// and the profile's cookies go only to granted cookie hosts.
class LiveFetcher {
 public:
  struct Limits {
    size_t max_fetches = 10;
    size_t max_response_bytes = 2 * 1024 * 1024;
    int max_redirects = 5;
  };

  using Callback =
      base::OnceCallback<void(base::expected<LiveFetchResponse, LiveError>)>;

  LiveFetcher(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
              LiveAllowedHosts allowed,
              bool cookies_granted,
              Limits limits);
  LiveFetcher(const LiveFetcher&) = delete;
  LiveFetcher& operator=(const LiveFetcher&) = delete;
  // Cancels requests in flight; their callbacks don't run.
  ~LiveFetcher();

  void Fetch(const LiveFetchRequest& request, Callback callback);

  const std::vector<LiveFetchRecord>& fetches() const { return fetches_; }

 private:
  struct Pending;

  void OnRedirect(Pending* pending, const GURL& new_url);
  void OnComplete(Pending* pending, std::optional<std::string> body);
  void Finish(Pending* pending,
              base::expected<LiveFetchResponse, LiveError> result);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const LiveAllowedHosts allowed_;
  const bool cookies_granted_;
  const Limits limits_;
  std::vector<LiveFetchRecord> fetches_;
  std::vector<std::unique_ptr<Pending>> pending_;
  base::WeakPtrFactory<LiveFetcher> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_LIVE_FOLDERS_LIVE_FETCHER_H_
