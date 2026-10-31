// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ARC_IMPORT_PROFILE_COPY_H_
#define COMPASS_BROWSER_ARC_IMPORT_PROFILE_COPY_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"

namespace compass::arc {

// Turns a value Arc encrypted into Compass's encryption: decrypts it with
// Arc's key and encrypts it with Compass's. Returns nullopt if it can't be
// decrypted.
using Reencrypt =
    base::RepeatingCallback<std::optional<std::string>(std::string_view)>;

struct CopyResult {
  bool ok = false;
  // Encrypted values now in Compass's encryption.
  int reencrypted = 0;
  // Cookies Arc's key couldn't decrypt, left out.
  int dropped_cookies = 0;
};

// Copies the Arc profile folder |from| to |to|, a new folder (it must not
// exist), and the disk cache |from_cache| (may not exist) to |to_cache|,
// leaving out lock files and Arc's open windows (Compass restores from its
// sidebar). Then re-encrypts every encrypted value of the profile's databases
// (cookies, passwords, cards, tokens) with |reencrypt|. Blocking; Arc must not
// be running (its databases would be in use). On failure, |to| is removed.
// See docs/design/arc-import.md, "Encryption".
CopyResult CopyProfile(const base::FilePath& from,
                       const base::FilePath& to,
                       const base::FilePath& from_cache,
                       const base::FilePath& to_cache,
                       const Reencrypt& reencrypt);

}  // namespace compass::arc

#endif  // COMPASS_BROWSER_ARC_IMPORT_PROFILE_COPY_H_
