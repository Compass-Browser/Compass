// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The parts of the Arc importer that talk to Arc itself, for platforms other
// than macOS (see arc_importer_mac.mm). Arc's profiles aren't found there
// (GetArcPaths() is empty), so these only keep the testing hooks working.

#include <optional>
#include <string>
#include <utility>

#include "base/no_destructor.h"
#include "compass/browser/ui/arc_import/arc_importer.h"

namespace compass::arc {

namespace {

std::optional<std::optional<std::string>>& PasswordForTesting() {
  static base::NoDestructor<std::optional<std::optional<std::string>>> value;
  return *value;
}

std::optional<bool>& RunningForTesting() {
  static std::optional<bool> running;
  return running;
}

}  // namespace

void SetArcKeychainPasswordForTesting(std::optional<std::string> password) {
  PasswordForTesting() = std::move(password);
}

std::optional<std::string> ReadArcKeychainPassword() {
  if (PasswordForTesting()) {
    return *PasswordForTesting();
  }
  return std::nullopt;
}

void SetArcRunningForTesting(std::optional<bool> running) {
  RunningForTesting() = running;
}

bool IsArcRunning() {
  return RunningForTesting().value_or(false);
}

bool QuitArc() {
  return false;
}

}  // namespace compass::arc
