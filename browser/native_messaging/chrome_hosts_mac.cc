// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/native_messaging/chrome_hosts_mac.h"

#include <string>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/logging.h"
#include "base/path_service.h"

namespace compass {
namespace {

// Hosts whose manifest Compass takes from Chrome's folder.
constexpr const char* kHostsFromChrome[] = {
    "com.1password.1password",
};

// Manifests are a few hundred bytes; anything bigger isn't one.
constexpr size_t kMaxManifestSize = 64 * 1024;

}  // namespace

void CopyNativeMessagingHostsFromChrome(const base::FilePath& chrome_dir,
                                        const base::FilePath& compass_dir) {
  for (const char* host : kHostsFromChrome) {
    const std::string file = std::string(host) + ".json";
    std::string chrome_manifest;
    if (!base::ReadFileToStringWithMaxSize(chrome_dir.Append(file),
                                           &chrome_manifest,
                                           kMaxManifestSize)) {
      continue;
    }
    const base::FilePath target = compass_dir.Append(file);
    std::string compass_manifest;
    if (base::ReadFileToStringWithMaxSize(target, &compass_manifest,
                                          kMaxManifestSize) &&
        compass_manifest == chrome_manifest) {
      continue;
    }
    if (!base::CreateDirectory(compass_dir) ||
        !base::ImportantFileWriter::WriteFileAtomically(target,
                                                        chrome_manifest)) {
      LOG(WARNING) << "Couldn't copy the native messaging host " << host
                   << " from Chrome's folder";
    }
  }
}

void CopyNativeMessagingHostsFromChrome(const base::FilePath& compass_dir) {
  base::FilePath app_support;
  if (!base::PathService::Get(base::DIR_APP_DATA, &app_support)) {
    return;
  }
  CopyNativeMessagingHostsFromChrome(
      app_support.Append("Google").Append("Chrome").Append(
          "NativeMessagingHosts"),
      compass_dir);
}

}  // namespace compass
