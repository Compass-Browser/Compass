// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_DEVTOOLS_FORMAT_H_
#define COMPASS_BROWSER_CLAUDE_DEVTOOLS_FORMAT_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "base/values.h"

// How the developer tools (docs/design/claude.md, "Developer tools") write
// what DevTools gives them for Claude: sizes, times, headers, and secrets
// hidden unless the user asked to see them.
namespace compass::claude::devtools_format {

// Whether a header, cookie, storage key or form field named |name| carries
// credentials: Authorization, Cookie, Set-Cookie, and names with "token",
// "secret", "password", "session", "auth", "credential" and the like.
bool IsSecretName(std::string_view name);

// Whether |value| looks like a credential whatever its name: a JWT, a
// "Bearer …" or "Basic …" value.
bool LooksSecret(std::string_view value);

// What stands for a hidden value: "<hidden, 32 characters>".
std::string Hidden(std::string_view value);

// |text| with the values of secret-named fields hidden: form and query
// fields ("access_token=…") and JSON members ("password": "…").
std::string MaskSecretsInText(std::string_view text);

// |headers| (name → value, as DevTools gives them) as "name: value" lines,
// sorted by name; credential values hidden unless |show_secrets|.
std::string FormatHeaders(const base::DictValue& headers, bool show_secrets);

// "512 B", "3.2 kB", "1.4 MB".
std::string FormatBytes(double bytes);

// "120 ms", "1.4 s".
std::string FormatDuration(double seconds);

// |text| cut to at most |max_bytes| (on a UTF-8 character boundary), with
// "…" when cut.
std::string Cut(std::string_view text, size_t max_bytes);

// The part of |text| from byte |offset| (moved forward to a character
// boundary), at most |max_bytes| long; |next| gets where the next part
// starts, or 0 when this is the last.
std::string Part(std::string_view text,
                 size_t offset,
                 size_t max_bytes,
                 size_t* next);

// Whether a body of |mime_type| is text Claude can read.
bool IsTextMimeType(std::string_view mime_type);

}  // namespace compass::claude::devtools_format

#endif  // COMPASS_BROWSER_CLAUDE_DEVTOOLS_FORMAT_H_
