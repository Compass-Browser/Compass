// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMMON_CLEAN_URL_H_
#define COMPASS_COMMON_CLEAN_URL_H_

class GURL;

namespace compass {

// |url| without the query parameters that only track where a link was
// shared or clicked, as Firefox's "Copy Clean Link" removes them: its
// strip-on-share lists (//compass/third_party/strip_on_share), global and
// per site. Like Firefox, also cleans an http(s) URL held in a parameter's
// value (?url=https://…), one level deep. Every other parameter keeps its
// order and spelling; a query left empty goes, "?" included. The fragment
// stays. URLs other than http and https come back unchanged.
GURL RemoveTrackingParams(const GURL& url);

}  // namespace compass

#endif  // COMPASS_COMMON_CLEAN_URL_H_
