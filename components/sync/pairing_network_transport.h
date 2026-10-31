// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_PAIRING_NETWORK_TRANSPORT_H_
#define COMPASS_COMPONENTS_SYNC_PAIRING_NETWORK_TRANSPORT_H_

#include <list>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "compass/components/sync/pairing.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace compass::sync {

// Talks to the pairing endpoints of a Compass sync server, under the sync
// URL's path (e.g. http://host:8295/v2 -> http://host:8295/v2/compass/pair).
class NetworkPairingTransport : public PairingTransport {
 public:
  // A's sync token, made fresh for each request. Unused on B.
  using TokenGetter = base::RepeatingCallback<std::string()>;

  NetworkPairingTransport(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      const GURL& server_url,
      TokenGetter token_getter);
  NetworkPairingTransport(const NetworkPairingTransport&) = delete;
  NetworkPairingTransport& operator=(const NetworkPairingTransport&) = delete;
  ~NetworkPairingTransport() override;

  // PairingTransport:
  void Open(OpenCallback callback) override;
  void Join(int slot, JoinCallback callback) override;
  void Send(int slot,
            PairingRole from,
            std::vector<uint8_t> data,
            StatusCallback callback) override;
  void Receive(int slot,
               PairingRole reader,
               int after,
               ReceiveCallback callback) override;
  void Close(int slot, PairingRole by, PairingClosedReason reason) override;

  // <server path>/compass/pair[/<suffix>].
  static GURL MakePairUrl(const GURL& server_url, std::string_view suffix);
  // Why a 410 reply says its slot closed; kUnknown for anything else.
  static PairingClosedReason ClosedReasonOf(
      int status,
      const std::optional<base::DictValue>& reply);

 private:
  using Loaders = std::list<std::unique_ptr<network::SimpleURLLoader>>;
  // The status, and the reply's JSON object if it has one.
  using ResponseCallback =
      base::OnceCallback<void(int status, std::optional<base::DictValue>)>;

  std::unique_ptr<network::SimpleURLLoader> MakeLoader(
      const std::string& method,
      const GURL& url,
      std::optional<PairingRole> auth,
      std::optional<std::string> body);
  void Request(const std::string& method,
               const GURL& url,
               std::optional<PairingRole> auth,
               std::optional<std::string> body,
               ResponseCallback callback);
  void OnResponse(Loaders::iterator it,
                  ResponseCallback callback,
                  std::optional<std::string> body);

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  const GURL server_url_;
  TokenGetter token_getter_;
  // B's token from the join.
  std::string b_token_;
  Loaders loaders_;

  base::WeakPtrFactory<NetworkPairingTransport> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_PAIRING_NETWORK_TRANSPORT_H_
