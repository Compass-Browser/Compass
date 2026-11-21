// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_DEV_MODE_NETWORK_LOG_BUFFER_H_
#define COMPASS_BROWSER_DEV_MODE_NETWORK_LOG_BUFFER_H_

#include <cstddef>
#include <list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/function_ref.h"
#include "base/values.h"

namespace compass::dev_mode {

// The DevTools protocol's Network events of a page in development mode,
// kept while DevTools is closed so its Network panel can show them once it
// opens (docs/design/dev-mode.md, "Requests before DevTools"). Grouped by
// request; the oldest request goes first when there are too many.
class NetworkLogBuffer {
 public:
  // Requests kept, at most.
  static constexpr size_t kMaxRequests = 1000;
  // Events kept per request, at most: a WebSocket's frames stop being kept,
  // its other events still are.
  static constexpr size_t kMaxEventsPerRequest = 200;

  NetworkLogBuffer();
  NetworkLogBuffer(const NetworkLogBuffer&) = delete;
  NetworkLogBuffer& operator=(const NetworkLogBuffer&) = delete;
  ~NetworkLogBuffer();

  // Whether |method| is an event the buffer keeps. Network.dataReceived
  // isn't: the panel takes sizes from loadingFinished, and there can be
  // thousands.
  static bool Keeps(std::string_view method);

  // Keeps the event, if Keeps() and it names a request.
  void Add(const std::string& method, const base::DictValue& params);
  void Clear();

  // Each kept event, requests in the order they started.
  void ForEach(base::FunctionRef<void(const std::string& method,
                                      const base::DictValue& params)> visit)
      const;

  bool Contains(const std::string& request_id) const;
  size_t request_count() const { return requests_.size(); }
  bool empty() const { return requests_.empty(); }

 private:
  struct Request {
    Request();
    Request(Request&&);
    Request& operator=(Request&&);
    ~Request();

    std::string id;
    std::vector<std::pair<std::string, base::DictValue>> events;
  };

  std::list<Request> requests_;
  std::map<std::string, std::list<Request>::iterator, std::less<>> index_;
};

}  // namespace compass::dev_mode

#endif  // COMPASS_BROWSER_DEV_MODE_NETWORK_LOG_BUFFER_H_
