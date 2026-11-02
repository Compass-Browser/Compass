// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_CREDENTIALS_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_CREDENTIALS_H_

#include <optional>
#include <string>

#include "base/functional/callback_forward.h"

class Profile;

namespace compass::claude {

// A profile's API key. It's stored encrypted with the OS's safe storage
// (os_crypt_async: the "Compass Safe Storage" Keychain item on macOS) in the
// profile's prefs, and only the browser process ever reads it: pages get
// KeyHint() at most. See docs/design/claude.md, "Settings".
class ClaudeCredentials {
 public:
  // Saves |key|, trimmed; an empty one removes the saved key. |callback|
  // tells whether it was saved (false if the OS's storage refused).
  static void SetApiKey(Profile* profile,
                        const std::string& key,
                        base::OnceCallback<void(bool)> callback);

  // The saved key, or null if none is saved or it can't be decrypted (the
  // OS's storage was reset).
  static void GetApiKey(
      Profile* profile,
      base::OnceCallback<void(std::optional<std::string>)> callback);

  static bool HasApiKey(Profile* profile);
  // KeyHint() of the saved key; empty if none.
  static std::string GetApiKeyHint(Profile* profile);
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_CREDENTIALS_H_
