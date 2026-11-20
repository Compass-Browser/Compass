// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_DEVICE_STATE_H_
#define COMPASS_COMPONENTS_SYNC_DEVICE_STATE_H_

#include <string_view>

#include "base/strings/string_util.h"

namespace compass::sync {

// A device removed from the chain on another device (docs/design/sync.md,
// "Removing a device"): the server answers each of its requests (/command/,
// events, push, quota) with HTTP 403 {"error":"device_removed"} and this
// header set to "removed".
inline constexpr char kDeviceStateHeader[] = "X-Compass-Device";
inline constexpr char kDeviceStateRemoved[] = "removed";

// Whether a reply with |http_status| and the kDeviceStateHeader value
// |device_header| (empty if absent) says this device was removed.
inline bool IsDeviceRemovedReply(int http_status,
                                 std::string_view device_header) {
  return http_status == 403 &&
         base::EqualsCaseInsensitiveASCII(
             base::TrimWhitespaceASCII(device_header, base::TRIM_ALL),
             kDeviceStateRemoved);
}

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_DEVICE_STATE_H_
