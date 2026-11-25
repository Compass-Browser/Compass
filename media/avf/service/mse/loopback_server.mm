// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/service/mse/loopback_server.h"

#import <Foundation/Foundation.h>
#include <Network/Network.h>

#include <optional>
#include <string_view>

#include "base/containers/span.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace compass::avf::mse {
namespace {

constexpr size_t kMaxRequestSize = 16 * 1024;

std::optional<std::pair<size_t, size_t>> ParseRange(std::string_view value,
                                                    size_t size) {
  // "bytes=first-last" or "bytes=first-".
  if (!base::StartsWith(value, "bytes=") || !size) {
    return std::nullopt;
  }
  std::vector<std::string_view> parts = base::SplitStringPiece(
      value.substr(6), "-", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  size_t first, last = size - 1;
  if (parts.size() != 2 || !base::StringToSizeT(parts[0], &first) ||
      (!parts[1].empty() && !base::StringToSizeT(parts[1], &last))) {
    return std::nullopt;
  }
  last = std::min(last, size - 1);
  if (first > last) {
    return std::nullopt;
  }
  return std::make_pair(first, last);
}

}  // namespace

struct LoopbackServer::Impl {
  dispatch_queue_t __strong queue;
  nw_listener_t __strong listener;
};

}  // namespace compass::avf::mse

// One connection's request buffer; requests are handled one at a time.
@interface CompassLoopbackConnection : NSObject
- (instancetype)initWithConnection:(nw_connection_t)connection
                            server:(compass::avf::mse::LoopbackServer*)server
                             queue:(dispatch_queue_t)queue;
- (void)start;
@end

@implementation CompassLoopbackConnection {
  nw_connection_t __strong _connection;
  raw_ptr<compass::avf::mse::LoopbackServer> _server;  // Never deleted.
  NSMutableData* __strong _buffer;
}

- (instancetype)initWithConnection:(nw_connection_t)connection
                            server:(compass::avf::mse::LoopbackServer*)server
                             queue:(dispatch_queue_t)queue {
  if ((self = [super init])) {
    _connection = connection;
    _server = server;
    _buffer = [NSMutableData data];
    nw_connection_set_queue(_connection, queue);
  }
  return self;
}

- (void)start {
  // The handler keeps `self` alive until the connection ends.
  nw_connection_set_state_changed_handler(
      _connection, ^(nw_connection_state_t state, nw_error_t) {
        if (state == nw_connection_state_ready) {
          [self receive];
        } else if (state == nw_connection_state_failed) {
          nw_connection_cancel(self->_connection);
        } else if (state == nw_connection_state_cancelled) {
          nw_connection_set_state_changed_handler(self->_connection, nil);
        }
      });
  nw_connection_start(_connection);
}

- (void)receive {
  nw_connection_receive(
      _connection, 1, compass::avf::mse::kMaxRequestSize,
      ^(dispatch_data_t content, nw_content_context_t, bool is_complete,
        nw_error_t error) {
        if (content) {
          dispatch_data_apply(content, ^bool(dispatch_data_t, size_t,
                                             const void* bytes, size_t size) {
            [self->_buffer appendBytes:bytes length:size];
            return true;
          });
        }
        while ([self handleRequest]) {
        }
        if (error || is_complete || self->_buffer.length > compass::avf::mse::kMaxRequestSize) {
          nw_connection_cancel(self->_connection);
          return;
        }
        [self receive];
      });
}

// Handles one complete request in the buffer, if there is one.
- (BOOL)handleRequest {
  std::string_view buffered(static_cast<const char*>(_buffer.bytes),
                            _buffer.length);
  size_t end = buffered.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return NO;
  }
  std::string request(buffered.substr(0, end));
  [_buffer replaceBytesInRange:NSMakeRange(0, end + 4) withBytes:nullptr
                        length:0];

  std::vector<std::string> lines = base::SplitStringUsingSubstr(
      request, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  std::vector<std::string> request_line =
      lines.empty() ? std::vector<std::string>()
                    : base::SplitString(lines[0], " ", base::TRIM_WHITESPACE,
                                        base::SPLIT_WANT_NONEMPTY);
  std::string range;
  for (size_t i = 1; i < lines.size(); ++i) {
    size_t colon = lines[i].find(':');
    if (colon != std::string::npos &&
        base::EqualsCaseInsensitiveASCII(lines[i].substr(0, colon), "range")) {
      range = std::string(base::TrimWhitespaceASCII(lines[i].substr(colon + 1),
                                                    base::TRIM_ALL));
    }
  }

  bool head = !request_line.empty() && request_line[0] == "HEAD";
  scoped_refptr<base::RefCountedBytes> body;
  std::string content_type;
  bool found = request_line.size() >= 2 &&
               (request_line[0] == "GET" || head) &&
               _server->Lookup(request_line[1], body, content_type);
  if (!found) {
    [self send:"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n" body:nullptr
          from:0
          size:0];
    return YES;
  }
  size_t size = body->as_vector().size();
  size_t from = 0, length = size;
  std::string status = "200 OK";
  std::string extra;
  if (!range.empty()) {
    if (auto parsed = compass::avf::mse::ParseRange(range, size)) {
      from = parsed->first;
      length = parsed->second - parsed->first + 1;
      status = "206 Partial Content";
      extra = base::StringPrintf("Content-Range: bytes %zu-%zu/%zu\r\n",
                                 parsed->first, parsed->second, size);
    }
  }
  std::string header = base::StringPrintf(
      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Cache-Control: no-store\r\nAccept-Ranges: bytes\r\n%s\r\n",
      status.c_str(), content_type.c_str(), length, extra.c_str());
  [self send:header body:head ? nullptr : body from:from size:length];
  return YES;
}

- (void)send:(const std::string&)header
        body:(scoped_refptr<base::RefCountedBytes>)body
        from:(size_t)from
        size:(size_t)size {
  dispatch_data_t data =
      dispatch_data_create(header.data(), header.size(), nullptr,
                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  if (body && size) {
    // The block keeps the bytes alive while Network.framework sends them.
    __block scoped_refptr<base::RefCountedBytes> keep = body;
    dispatch_data_t payload = dispatch_data_create(
        base::span(body->as_vector()).subspan(from, size).data(), size,
        nullptr, ^{
          keep = nullptr;
        });
    data = dispatch_data_create_concat(data, payload);
  }
  nw_connection_send(_connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT,
                     false, ^(nw_error_t) {
                     });
}

@end

namespace compass::avf::mse {

// static
LoopbackServer* LoopbackServer::Get() {
  static LoopbackServer* server = [] {
    auto* instance = new LoopbackServer();
    if (!instance->Start()) {
      LOG(ERROR) << "FairPlay MSE: can't listen on 127.0.0.1";
      delete instance;
      return static_cast<LoopbackServer*>(nullptr);
    }
    return instance;
  }();
  return server;
}

LoopbackServer::LoopbackServer() : impl_(std::make_unique<Impl>()) {}

LoopbackServer::~LoopbackServer() {
  if (impl_->listener) {
    nw_listener_cancel(impl_->listener);
  }
}

bool LoopbackServer::Start() {
  impl_->queue = dispatch_queue_create("org.compass.avf.loopback",
                                       DISPATCH_QUEUE_SERIAL);
  nw_parameters_t parameters = nw_parameters_create_secure_tcp(
      NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
  nw_parameters_set_local_endpoint(parameters,
                                   nw_endpoint_create_host("127.0.0.1", "0"));
  impl_->listener = nw_listener_create(parameters);
  if (!impl_->listener) {
    return false;
  }
  nw_listener_set_queue(impl_->listener, impl_->queue);

  // Starting takes a moment; the first MSE session waits for it. The blocks
  // keep the semaphore (and the __block variables) alive.
  dispatch_semaphore_t ready = dispatch_semaphore_create(0);
  __block bool ok = false;
  __block uint16_t port = 0;
  nw_listener_t listener = impl_->listener;
  nw_listener_set_state_changed_handler(
      listener, ^(nw_listener_state_t state, nw_error_t) {
        if (state == nw_listener_state_ready) {
          ok = true;
          port = nw_listener_get_port(listener);
          dispatch_semaphore_signal(ready);
        } else if (state == nw_listener_state_failed ||
                   state == nw_listener_state_cancelled) {
          dispatch_semaphore_signal(ready);
        }
      });
  dispatch_queue_t queue = impl_->queue;
  nw_listener_set_new_connection_handler(listener, ^(nw_connection_t connection) {
    [[[CompassLoopbackConnection alloc] initWithConnection:connection
                                                    server:this
                                                     queue:queue] start];
  });
  nw_listener_start(listener);
  dispatch_semaphore_wait(ready,
                          dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
  if (!ok) {
    return false;
  }
  port_ = port;
  return true;
}

GURL LoopbackServer::Register(scoped_refptr<HlsSession> session) {
  std::string token = base::ToLowerASCII(base::HexEncode(base::RandBytesAsVector(16)));
  {
    base::AutoLock lock(lock_);
    sessions_[token] = std::move(session);
  }
  return GURL(base::StringPrintf("http://127.0.0.1:%u/%s/index.m3u8", port_,
                                 token.c_str()));
}

void LoopbackServer::Unregister(const GURL& url) {
  std::vector<std::string> parts = base::SplitString(
      url.path(), "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.empty()) {
    return;
  }
  base::AutoLock lock(lock_);
  sessions_.erase(parts[0]);
}

bool LoopbackServer::Lookup(const std::string& path,
                            scoped_refptr<base::RefCountedBytes>& body,
                            std::string& content_type) {
  std::vector<std::string> parts = base::SplitString(
      path, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.size() != 2) {
    return false;
  }
  scoped_refptr<HlsSession> session;
  {
    base::AutoLock lock(lock_);
    auto it = sessions_.find(parts[0]);
    if (it == sessions_.end()) {
      return false;
    }
    session = it->second;
  }
  if (parts[1] == "index.m3u8") {
    std::optional<std::string> playlist = session->Playlist();
    if (!playlist) {
      return false;
    }
    body = base::MakeRefCounted<base::RefCountedBytes>(
        std::vector<uint8_t>(playlist->begin(), playlist->end()));
    content_type = "application/vnd.apple.mpegurl";
    return true;
  }
  body = session->Resource(parts[1]);
  content_type = "video/mp4";
  return !!body;
}

}  // namespace compass::avf::mse
