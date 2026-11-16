// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BOOSTS_BOOST_SHARE_H_
#define COMPASS_BROWSER_BOOSTS_BOOST_SHARE_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/types/expected.h"
#include "compass/browser/boosts/boost_store.h"

class GURL;

namespace compass::boosts {

// Sharing a Boost (docs/design/boosts.md, "Sharing"), the same on every
// Compass client:
//
// - The shared JSON, UTF-8: {"v":1, "name", "match", "css", "js",
//   "js_world"}; css, js and js_world may be absent. No id, author, times
//   or enabled. Unknown keys are ignored; a "v" other than 1 is refused.
// - A link: kShareLinkPrefix + base64url (no padding) of the raw DEFLATE
//   (no zlib or gzip header) of the JSON. The payload is in the fragment
//   only, so it never reaches a server.
// - A file: <name>.compassboost, the JSON pretty-printed, uncompressed.

inline constexpr char kShareLinkPrefix[] = "https://compassbrowser.org/boost#";
// The version this client writes and reads.
inline constexpr int kShareVersion = 1;
// A longer link is offered as a file only.
inline constexpr size_t kMaxShareLinkBytes = 64 * 1024;
// What a link or file may decompress to, at most: a decompression bomb
// guard. Comfortably over kMaxBoostBytes with the JSON around it.
inline constexpr size_t kMaxSharedBoostBytes = 512 * 1024;
inline constexpr char kShareFileExtension[] = "compassboost";
inline constexpr char kShareFileMimeType[] =
    "application/vnd.compassbrowser.boost+json";

// Why a shared Boost can't be added.
struct ShareError {
  enum class Kind {
    // Not a shared Boost: base64url, DEFLATE or JSON broken, or a key of
    // the wrong type.
    kInvalid,
    // A "v" other than kShareVersion.
    kNewer,
    // Over kMaxSharedBoostBytes.
    kTooBig,
    // Refused by the rules for saving a Boost (NormalizeBoost()).
    kRefused,
  };
  Kind kind;
  // For the user.
  std::string message;
};

// The shared JSON for |boost|: compact for a link, pretty for a file.
std::string ToSharedJson(const Boost& boost, bool pretty);

// The link's fragment for |boost| (without "#").
std::string EncodeShareFragment(const Boost& boost);
// kShareLinkPrefix + EncodeShareFragment(). Check its size against
// kMaxShareLinkBytes.
std::string ShareLink(const Boost& boost);

// The Boost a link's fragment shares, checked as a save would check it:
// no id, enabled, author kAuthorShared.
base::expected<Boost, ShareError> DecodeShareFragment(
    std::string_view fragment);
// The same for a file's contents (or a link's JSON once inflated).
base::expected<Boost, ShareError> ParseSharedJson(std::string_view json);

// The fragment of a share link: an https://compassbrowser.org/boost URL
// (also www.compassbrowser.org, and /boost/ and /boost.html) with a
// non-empty fragment. Null otherwise.
std::optional<std::string> ShareFragmentFromUrl(const GURL& url);

// The Boosts settings section's import preview for a link's fragment:
// chrome://settings/boosts?import=<fragment>.
GURL ImportPreviewUrl(std::string_view fragment);

// A file name for sharing |boost|: its name without characters file
// systems refuse, and the extension.
std::string ShareFileName(const Boost& boost);

}  // namespace compass::boosts

#endif  // COMPASS_BROWSER_BOOSTS_BOOST_SHARE_H_
