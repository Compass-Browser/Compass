// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/os_user_name.h"

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#define SECURITY_WIN32
#include <security.h>

#include "base/strings/utf_string_conversions.h"
#elif BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)
#include <pwd.h>
#include <unistd.h>
#endif

namespace compass::rooms {

std::string GetOsUserFullName() {
#if BUILDFLAG(IS_WIN)
  wchar_t name[256];
  ULONG size = std::size(name);
  if (GetUserNameExW(NameDisplay, name, &size) && size > 0) {
    return base::WideToUTF8(std::wstring(name, size));
  }
  return std::string();
#elif BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)
  const struct passwd* entry = getpwuid(getuid());
  if (!entry || !entry->pw_gecos) {
    return std::string();
  }
  // GECOS: "Full Name,room,phone,...".
  std::string gecos(entry->pw_gecos);
  return gecos.substr(0, gecos.find(','));
#else
  return std::string();
#endif
}

}  // namespace compass::rooms
