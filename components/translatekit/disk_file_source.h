// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_TRANSLATEKIT_DISK_FILE_SOURCE_H_
#define COMPASS_COMPONENTS_TRANSLATEKIT_DISK_FILE_SOURCE_H_

#include <optional>
#include <string>

#include "compass/components/translatekit/engine.h"

namespace compass::translatekit {

// Package files straight from the disk: for the CLI and tests. The library
// itself never opens files (the sandbox doesn't let it).
class DiskFileSource : public FileSource {
 public:
  bool Exists(const std::string& path) override;
  std::optional<std::string> Read(const std::string& path) override;
};

}  // namespace compass::translatekit

#endif  // COMPASS_COMPONENTS_TRANSLATEKIT_DISK_FILE_SOURCE_H_
