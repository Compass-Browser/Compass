// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/devtools_format.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "third_party/re2/src/re2/re2.h"

namespace compass::claude::devtools_format {

namespace {

// Parts of names that carry credentials. "auth" covers Authorization and
// x-auth-*; "passw" password and passwd.
constexpr std::string_view kSecretParts[] = {
    "token",       "secret",      "passw",   "pwd",
    "session",     "auth",        "credential", "apikey",
    "api-key",     "api_key",     "private-key", "private_key",
    "jwt",         "signature",   "cookie",
};

// The same, as a regular expression's alternatives.
constexpr char kSecretPattern[] =
    "(?:token|secret|passw|pwd|session|auth|credential|api[-_]?key|"
    "private[-_]?key|jwt|signature|cookie)";

const re2::RE2& FormField() {
  static const base::NoDestructor<re2::RE2> re(base::StrCat(
      {"(?i)((?:^|[?&;])[^=&;?/\\s]*", kSecretPattern,
       "[^=&;?/\\s]*=)[^&;\\s#]*"}));
  return *re;
}

const re2::RE2& JsonMember() {
  static const base::NoDestructor<re2::RE2> re(
      base::StrCat({"(?i)(\"[^\"\\\\]*", kSecretPattern,
                    "[^\"\\\\]*\"\\s*:\\s*)\"(?:[^\"\\\\]|\\\\.)*\""}));
  return *re;
}

const re2::RE2& Jwt() {
  static const base::NoDestructor<re2::RE2> re(
      "eyJ[A-Za-z0-9_-]{5,}\\.[A-Za-z0-9_-]{5,}\\.[A-Za-z0-9_-]*");
  return *re;
}

// Moves |offset| forward to the start of a UTF-8 character.
size_t ToCharBoundary(std::string_view text, size_t offset) {
  while (offset < text.size() &&
         (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) {
    ++offset;
  }
  return offset;
}

// The longest prefix of |text| of at most |max_bytes| that ends on a
// character boundary.
size_t PrefixLength(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return text.size();
  }
  size_t length = max_bytes;
  while (length > 0 &&
         (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
    --length;
  }
  return length;
}

}  // namespace

bool IsSecretName(std::string_view name) {
  const std::string lower = base::ToLowerASCII(name);
  return std::ranges::any_of(kSecretParts, [&](std::string_view part) {
    return lower.find(part) != std::string::npos;
  });
}

bool LooksSecret(std::string_view value) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(value, base::TRIM_ALL);
  if (base::StartsWith(trimmed, "Bearer ",
                       base::CompareCase::INSENSITIVE_ASCII) ||
      base::StartsWith(trimmed, "Basic ",
                       base::CompareCase::INSENSITIVE_ASCII)) {
    return true;
  }
  return re2::RE2::PartialMatch(trimmed, Jwt());
}

std::string Hidden(std::string_view value) {
  const size_t characters = base::UTF8ToUTF16(value).size();
  return base::StrCat({"<hidden, ", base::NumberToString(characters),
                       characters == 1 ? " character>" : " characters>"});
}

std::string MaskSecretsInText(std::string_view text) {
  std::string masked(text);
  re2::RE2::GlobalReplace(&masked, FormField(), "\\1<hidden>");
  re2::RE2::GlobalReplace(&masked, JsonMember(), "\\1\"<hidden>\"");
  re2::RE2::GlobalReplace(&masked, Jwt(), "<hidden JWT>");
  return masked;
}

std::string FormatHeaders(const base::DictValue& headers, bool show_secrets) {
  std::vector<std::pair<std::string, std::string>> lines;
  for (const auto [name, value] : headers) {
    std::string text = value.is_string() ? value.GetString() : std::string();
    if (!show_secrets && (IsSecretName(name) || LooksSecret(text))) {
      // Set-Cookie and Cookie may hold several, one per line.
      text = Hidden(text);
    } else {
      text = Cut(text, 500);
    }
    lines.emplace_back(base::ToLowerASCII(name), std::move(text));
  }
  std::ranges::sort(lines);
  std::string out;
  for (const auto& [name, value] : lines) {
    base::StrAppend(&out, {"  ", name, ": ", value, "\n"});
  }
  return out;
}

std::string FormatBytes(double bytes) {
  if (bytes < 1000) {
    return base::StringPrintf("%d B", static_cast<int>(bytes));
  }
  if (bytes < 1000 * 1000) {
    return base::StringPrintf("%.1f kB", bytes / 1000);
  }
  return base::StringPrintf("%.1f MB", bytes / (1000 * 1000));
}

std::string FormatDuration(double seconds) {
  if (seconds < 1) {
    return base::StringPrintf("%d ms", static_cast<int>(seconds * 1000 + 0.5));
  }
  return base::StringPrintf("%.1f s", seconds);
}

std::string Cut(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return std::string(text);
  }
  return base::StrCat({text.substr(0, PrefixLength(text, max_bytes)), "…"});
}

std::string Part(std::string_view text,
                 size_t offset,
                 size_t max_bytes,
                 size_t* next) {
  offset = ToCharBoundary(text, std::min(offset, text.size()));
  const std::string_view rest = text.substr(offset);
  const size_t length = PrefixLength(rest, max_bytes);
  *next = length < rest.size() ? offset + length : 0;
  return std::string(rest.substr(0, length));
}

bool IsTextMimeType(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  if (lower.starts_with("text/")) {
    return true;
  }
  for (std::string_view part :
       {"json", "javascript", "ecmascript", "xml", "x-www-form-urlencoded",
        "graphql", "csv", "yaml"}) {
    if (lower.find(part) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace compass::claude::devtools_format
