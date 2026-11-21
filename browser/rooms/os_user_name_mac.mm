// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/os_user_name.h"

#import <Foundation/Foundation.h>

#include "base/strings/sys_string_conversions.h"

namespace compass::rooms {

std::string GetOsUserFullName() {
  return base::SysNSStringToUTF8(NSFullUserName());
}

}  // namespace compass::rooms
