// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/boost_share.h"

#include <algorithm>
#include <utility>

#include "base/base64url.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "third_party/zlib/zlib.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace compass::boosts {

namespace {

constexpr std::string_view kShareHosts[] = {"compassbrowser.org",
                                            "www.compassbrowser.org"};
constexpr std::string_view kSharePaths[] = {"/boost", "/boost/",
                                            "/boost.html"};
constexpr char kImportPreview[] = "chrome://settings/boosts?import=";
constexpr char kFileNameFallback[] = "Boost";

ShareError Invalid() {
  return {ShareError::Kind::kInvalid,
          "This isn't a Boost that Compass can read. The link or file may "
          "be incomplete."};
}

ShareError TooBig() {
  return {ShareError::Kind::kTooBig,
          "This Boost is too big to add: shared Boosts are at most 512 KB."};
}

std::string DeflateRaw(std::string_view data) {
  z_stream stream = {};
  // windowBits -15: raw DEFLATE, no zlib header or checksum.
  if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return std::string();
  }
  std::string out(deflateBound(&stream, data.size()), '\0');
  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return result == Z_STREAM_END ? out : std::string();
}

// Inflates raw DEFLATE, stopping past kMaxSharedBoostBytes of output.
base::expected<std::string, ShareError> InflateRaw(std::string_view data) {
  z_stream stream = {};
  if (inflateInit2(&stream, -15) != Z_OK) {
    return base::unexpected(Invalid());
  }
  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());
  std::string out;
  char buffer[16 * 1024];
  int result = Z_OK;
  while (result == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(buffer);
    stream.avail_out = sizeof(buffer);
    result = inflate(&stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_STREAM_END) {
      break;
    }
    out.append(buffer, sizeof(buffer) - stream.avail_out);
    if (out.size() > kMaxSharedBoostBytes) {
      inflateEnd(&stream);
      return base::unexpected(TooBig());
    }
    if (result == Z_OK && stream.avail_in == 0 && stream.avail_out != 0) {
      // Out of input before the end of the stream: cut short.
      break;
    }
  }
  inflateEnd(&stream);
  if (result != Z_STREAM_END) {
    return base::unexpected(Invalid());
  }
  return out;
}

// A string under |key|: "" when absent, null when of another type.
std::optional<std::string> OptionalString(const base::DictValue& dict,
                                          std::string_view key) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return std::string();
  }
  if (!value->is_string()) {
    return std::nullopt;
  }
  return value->GetString();
}

}  // namespace

std::string ToSharedJson(const Boost& boost, bool pretty) {
  base::DictValue dict;
  dict.Set("v", kShareVersion);
  dict.Set("name", boost.name);
  dict.Set("match", boost.match);
  if (!boost.css.empty()) {
    dict.Set("css", boost.css);
  }
  if (!boost.js.empty()) {
    dict.Set("js", boost.js);
    dict.Set("js_world", boost.js_world.empty() ? kIsolatedWorld
                                                : boost.js_world);
  }
  std::string json;
  base::JSONWriter::WriteWithOptions(
      dict, pretty ? base::JSONWriter::OPTIONS_PRETTY_PRINT : 0, &json);
  return json;
}

std::string EncodeShareFragment(const Boost& boost) {
  std::string fragment;
  base::Base64UrlEncode(DeflateRaw(ToSharedJson(boost, /*pretty=*/false)),
                        base::Base64UrlEncodePolicy::OMIT_PADDING, &fragment);
  return fragment;
}

std::string ShareLink(const Boost& boost) {
  return base::StrCat({kShareLinkPrefix, EncodeShareFragment(boost)});
}

base::expected<Boost, ShareError> DecodeShareFragment(
    std::string_view fragment) {
  std::string compressed;
  if (fragment.empty() ||
      !base::Base64UrlDecode(fragment,
                             base::Base64UrlDecodePolicy::IGNORE_PADDING,
                             &compressed)) {
    return base::unexpected(Invalid());
  }
  base::expected<std::string, ShareError> json = InflateRaw(compressed);
  if (!json.has_value()) {
    return base::unexpected(std::move(json.error()));
  }
  return ParseSharedJson(*json);
}

base::expected<Boost, ShareError> ParseSharedJson(std::string_view json) {
  if (json.size() > kMaxSharedBoostBytes) {
    return base::unexpected(TooBig());
  }
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return base::unexpected(Invalid());
  }
  const base::DictValue& dict = value->GetDict();
  const base::Value* version = dict.Find("v");
  if (!version || !version->is_int()) {
    return base::unexpected(Invalid());
  }
  if (version->GetInt() != kShareVersion) {
    return base::unexpected(ShareError{
        ShareError::Kind::kNewer,
        "This Boost was made by a newer Compass. Update Compass to add it."});
  }
  const std::string* match = dict.FindString("match");
  std::optional<std::string> name = OptionalString(dict, "name");
  std::optional<std::string> css = OptionalString(dict, "css");
  std::optional<std::string> js = OptionalString(dict, "js");
  std::optional<std::string> world = OptionalString(dict, "js_world");
  if (!match || !name || !css || !js || !world) {
    return base::unexpected(Invalid());
  }
  Boost boost;
  boost.name = std::move(*name);
  boost.match = *match;
  boost.css = std::move(*css);
  boost.js = std::move(*js);
  boost.js_world = world->empty() ? kIsolatedWorld : std::move(*world);
  boost.enabled = true;
  boost.author = kAuthorShared;
  base::expected<Boost, std::string> normalized =
      NormalizeBoost(std::move(boost));
  if (!normalized.has_value()) {
    return base::unexpected(
        ShareError{ShareError::Kind::kRefused, normalized.error()});
  }
  return std::move(*normalized);
}

std::optional<std::string> ShareFragmentFromUrl(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIs(url::kHttpsScheme) ||
      !std::ranges::contains(kShareHosts, url.host()) || !url.has_ref() ||
      url.ref().empty() ||
      !std::ranges::contains(kSharePaths, url.path())) {
    return std::nullopt;
  }
  return std::string(url.ref());
}

GURL ImportPreviewUrl(std::string_view fragment) {
  // base64url needs no escaping in a query. Anything else can't be a shared
  // Boost: the preview gets nothing and says the link is broken.
  const bool base64url = std::ranges::all_of(fragment, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_' || c == '=';
  });
  return GURL(base::StrCat(
      {kImportPreview, base64url ? fragment : std::string_view()}));
}

std::string ShareFileName(const Boost& boost) {
  std::string name;
  for (char c : boost.name) {
    const bool refused = static_cast<unsigned char>(c) < 0x20 || c == 0x7f ||
                         std::string_view("/\\:*?\"<>|").find(c) !=
                             std::string_view::npos;
    name.push_back(refused ? '-' : c);
  }
  name = std::string(
      base::TrimString(name, " .-", base::TRIM_ALL));
  if (name.empty()) {
    name = kFileNameFallback;
  }
  return base::StrCat({name, ".", kShareFileExtension});
}

}  // namespace compass::boosts
