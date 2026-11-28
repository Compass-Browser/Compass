// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/translatekit/disk_file_source.h"

#include <fstream>
#include <iterator>
#include <sstream>

namespace compass::translatekit {

bool DiskFileSource::Exists(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return file.good();
}

std::optional<std::string> DiskFileSource::Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream bytes;
  bytes << file.rdbuf();
  return bytes.str();
}

}  // namespace compass::translatekit
