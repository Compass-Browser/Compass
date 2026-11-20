// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_COMPASS_DEVICE_REMOVER_H_
#define COMPASS_COMPONENTS_SYNC_COMPASS_DEVICE_REMOVER_H_

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace net {
class HttpResponseHeaders;
}  // namespace net

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace compass::sync {

// Whether |headers| (null: no reply) say the server removed this device
// (device_state.h).
bool IsDeviceRemovedResponse(const net::HttpResponseHeaders* headers);

// <server URL>/compass/devices/<cache_guid>?client_id=<client_id> (the
// server URL ends in /v2).
GURL MakeDeviceUrl(const GURL& server_url,
                   std::string_view cache_guid,
                   std::string_view client_id);

// Removes another device from the chain's account on its server
// (docs/design/sync.md, "Removing a device"): DELETE
// <server>/compass/devices/<its cache GUID>, signed with the account's token.
// The server deletes the device's DEVICE_INFO entity, which the other devices
// learn through sync, and refuses that cache GUID from then on.
class DeviceRemover {
 public:
  enum class Result {
    // 2xx: gone (also if it already was).
    kRemoved,
    // 409: the cache GUID is the asking device's own.
    kThisDevice,
    // 401 with X-Compass-Login: the server wants its sign-in first.
    kLoginRequired,
    // 401 otherwise: the server refused the token.
    kUnauthorized,
    // 403 with X-Compass-Device: removed: the asking device itself was
    // removed.
    kDeviceRemoved,
    // 404 or 405: a server without the endpoint.
    kUnsupported,
    // 400, any other status, or no answer.
    kFailed,
  };

  // A fresh sync token (SyncCredentials::MakeToken), or nullopt.
  using TokenGetter = base::RepeatingCallback<std::optional<std::string>()>;
  using Callback = base::OnceCallback<void(Result)>;

  DeviceRemover(scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
                const GURL& server_url,
                TokenGetter token_getter);
  DeviceRemover(const DeviceRemover&) = delete;
  DeviceRemover& operator=(const DeviceRemover&) = delete;
  ~DeviceRemover();

  // The status of a reply, as a Result. |headers| null: no reply.
  static Result ResultOf(const net::HttpResponseHeaders* headers);

  // Removes |cache_guid|; |client_id| is the asking device's own cache GUID.
  // Without a token or either GUID, nothing is sent and |callback| runs at
  // once with kFailed. Requests may overlap; destroying |this| drops them
  // without running their callbacks.
  void Remove(const std::string& cache_guid,
              const std::string& client_id,
              Callback callback);

  size_t pending() const { return loaders_.size(); }

 private:
  using Loaders = std::list<std::unique_ptr<network::SimpleURLLoader>>;
  void OnResponse(Loaders::iterator loader,
                  Callback callback,
                  std::optional<std::string> body);

  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const GURL server_url_;
  TokenGetter token_getter_;
  Loaders loaders_;
  base::WeakPtrFactory<DeviceRemover> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_COMPASS_DEVICE_REMOVER_H_
