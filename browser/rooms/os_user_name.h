// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ROOMS_OS_USER_NAME_H_
#define COMPASS_BROWSER_ROOMS_OS_USER_NAME_H_

#include <string>

namespace compass::rooms {

// The computer's user's full name ("Ada Lovelace"), the default name other
// members of a shared item see; "" where the system has none (Android).
std::string GetOsUserFullName();

}  // namespace compass::rooms

#endif  // COMPASS_BROWSER_ROOMS_OS_USER_NAME_H_
