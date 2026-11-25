// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_SERVICE_MSE_LOOPBACK_SERVER_H_
#define COMPASS_MEDIA_AVF_SERVICE_MSE_LOOPBACK_SERVER_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "compass/media/avf/service/mse/hls_session.h"
#include "url/gurl.h"

namespace compass::avf::mse {

// Serves HlsSessions to AVPlayer over HTTP on 127.0.0.1: AVPlayer loads HLS
// media segments only over the network, not through
// AVAssetResourceLoader (the FairPlay spike, docs/design/fairplay.md). Each
// session is under a random path; its segments are encrypted as the page's
// were, so another local process reading them gets nothing it couldn't from
// the page's own server. One per process, on Network.framework's queue.
class LoopbackServer {
 public:
  // Starts the server on first use; null if it can't listen.
  static LoopbackServer* Get();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  // The playlist's URL; `session` is served until Unregister(url).
  GURL Register(scoped_refptr<HlsSession> session);
  void Unregister(const GURL& url);

  // For the connection handlers: the body for `path` ("/<token>/<name>"),
  // and its content type. False if there's none.
  bool Lookup(const std::string& path,
              scoped_refptr<base::RefCountedBytes>& body,
              std::string& content_type);

 private:
  LoopbackServer();
  ~LoopbackServer();
  bool Start();

  struct Impl;  // ObjC objects.
  std::unique_ptr<Impl> impl_;
  uint16_t port_ = 0;

  base::Lock lock_;
  std::map<std::string, scoped_refptr<HlsSession>> sessions_ GUARDED_BY(lock_);
};

}  // namespace compass::avf::mse

#endif  // COMPASS_MEDIA_AVF_SERVICE_MSE_LOOPBACK_SERVER_H_
