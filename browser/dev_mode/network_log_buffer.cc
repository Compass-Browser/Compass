// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/dev_mode/network_log_buffer.h"

#include <algorithm>
#include <array>

#include "base/strings/string_util.h"

namespace compass::dev_mode {

namespace {

// The events of a request's life the Network panel builds its rows from.
constexpr auto kKeptEvents = std::to_array<std::string_view>({
    "Network.requestWillBeSent",
    "Network.requestWillBeSentExtraInfo",
    "Network.responseReceived",
    "Network.responseReceivedExtraInfo",
    "Network.responseReceivedEarlyHints",
    "Network.requestServedFromCache",
    "Network.resourceChangedPriority",
    "Network.loadingFinished",
    "Network.loadingFailed",
    "Network.eventSourceMessageReceived",
    "Network.webSocketCreated",
    "Network.webSocketWillSendHandshakeRequest",
    "Network.webSocketHandshakeResponseReceived",
    "Network.webSocketFrameSent",
    "Network.webSocketFrameReceived",
    "Network.webSocketFrameError",
    "Network.webSocketClosed",
    "Network.webTransportCreated",
    "Network.webTransportConnectionEstablished",
    "Network.webTransportClosed",
});

}  // namespace

NetworkLogBuffer::Request::Request() = default;
NetworkLogBuffer::Request::Request(Request&&) = default;
NetworkLogBuffer::Request& NetworkLogBuffer::Request::operator=(Request&&) =
    default;
NetworkLogBuffer::Request::~Request() = default;

NetworkLogBuffer::NetworkLogBuffer() = default;
NetworkLogBuffer::~NetworkLogBuffer() = default;

// static
bool NetworkLogBuffer::Keeps(std::string_view method) {
  return std::ranges::contains(kKeptEvents, method);
}

void NetworkLogBuffer::Add(const std::string& method,
                           const base::DictValue& params) {
  if (!Keeps(method)) {
    return;
  }
  // WebTransport names its request "transportId".
  const std::string* id = params.FindString("requestId");
  if (!id) {
    id = params.FindString("transportId");
  }
  if (!id || id->empty()) {
    return;
  }
  auto it = index_.find(*id);
  if (it == index_.end()) {
    if (requests_.size() >= kMaxRequests) {
      index_.erase(requests_.front().id);
      requests_.pop_front();
    }
    Request request;
    request.id = *id;
    requests_.push_back(std::move(request));
    it = index_.emplace(*id, std::prev(requests_.end())).first;
  }
  Request& request = *it->second;
  if (request.events.size() >= kMaxEventsPerRequest &&
      base::StartsWith(method, "Network.webSocketFrame")) {
    return;
  }
  request.events.emplace_back(method, params.Clone());
}

void NetworkLogBuffer::Clear() {
  index_.clear();
  requests_.clear();
}

void NetworkLogBuffer::ForEach(
    base::FunctionRef<void(const std::string& method,
                           const base::DictValue& params)> visit) const {
  for (const Request& request : requests_) {
    for (const auto& [method, params] : request.events) {
      visit(method, params);
    }
  }
}

bool NetworkLogBuffer::Contains(const std::string& request_id) const {
  return index_.contains(request_id);
}

}  // namespace compass::dev_mode
