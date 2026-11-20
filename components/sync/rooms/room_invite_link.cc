// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/rooms/room_invite_link.h"

#include <algorithm>
#include <cmath>

#include "base/base64url.h"
#include "base/json/json_reader.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "url/gurl.h"

namespace compass::sync::rooms {
namespace {

std::string B64(base::span<const uint8_t> bytes) {
  std::string out;
  base::Base64UrlEncode(bytes, base::Base64UrlEncodePolicy::OMIT_PADDING, &out);
  return out;
}

// Exactly |N| bytes of canonical, unpadded base64url.
template <size_t N>
std::optional<std::array<uint8_t, N>> FromB64(const std::string* text) {
  if (!text) {
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> bytes = base::Base64UrlDecode(
      *text, base::Base64UrlDecodePolicy::DISALLOW_PADDING);
  if (!bytes || bytes->size() != N || B64(*bytes) != *text) {
    return std::nullopt;
  }
  std::array<uint8_t, N> out;
  std::ranges::copy(*bytes, out.begin());
  return out;
}

// '"', '\' and control characters escaped; the rest as is (UTF-8).
void AppendJsonString(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          base::StringAppendF(&out, "\\u%04x", c);
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// See InviteLink::server_url. The host and port are read from the text
// itself, the same way on every client, before GURL has its say.
bool IsValidServerUrl(std::string_view text) {
  const bool https = text.starts_with("https://");
  if (!https && !text.starts_with("http://")) {
    return false;
  }
  constexpr std::string_view kAllowed =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "-._~:/[]!$&'()*+,;=%";
  if (text.find_first_not_of(kAllowed) != std::string_view::npos) {
    return false;
  }
  std::string_view authority = text.substr(https ? 8 : 7);
  authority = authority.substr(0, authority.find('/'));
  std::string_view host = authority;
  std::string_view port;
  if (authority.starts_with('[')) {
    const size_t end = authority.find(']');
    if (end == std::string_view::npos) {
      return false;
    }
    host = authority.substr(0, end + 1);
    port = authority.substr(end + 1);
  } else if (const size_t colon = authority.find(':');
             colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    port = authority.substr(colon);
  }
  if (host.empty()) {
    return false;
  }
  if (!port.empty()) {
    port.remove_prefix(1);  // ':' (or whatever follows ']')
    unsigned value = 0;
    if (!authority.substr(host.size()).starts_with(':') || port.empty() ||
        port.size() > 5 || !base::StringToUint(port, &value) || value == 0 ||
        value > 65535 || !std::ranges::all_of(port, base::IsAsciiDigit<char>)) {
      return false;
    }
  }
  if (!https && host != "localhost" && host != "127.0.0.1" && host != "[::1]") {
    return false;
  }
  const GURL url(text);
  return url.is_valid() && !url.host().empty() && !url.has_username() &&
         !url.has_password() && !url.has_query() && !url.has_ref();
}

bool IsValidInviteId(std::string_view id) {
  if (id.empty() || id.size() > kMaxInviteIdSize) {
    return false;
  }
  return std::ranges::all_of(id, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-';
  });
}

bool IsValidName(std::string_view name) {
  return name.size() <= kMaxRoomNameSize &&
         base::IsStringUTF8AllowingNoncharacters(name);
}

bool IsValid(const InviteLink& link) {
  return IsValidServerUrl(link.server_url) && IsValidInviteId(link.invite_id) &&
         IsValidName(link.name);
}

// A JSON number that is a whole uint32.
std::optional<uint32_t> GetEpoch(const base::Value* value) {
  if (!value) {
    return std::nullopt;
  }
  if (std::optional<int> i = value->GetIfInt()) {
    if (*i < 0) {
      return std::nullopt;
    }
    return static_cast<uint32_t>(*i);
  }
  if (std::optional<double> d = value->GetIfDouble()) {
    if (*d >= 0 && *d <= 4294967295.0 && std::floor(*d) == *d) {
      return static_cast<uint32_t>(*d);
    }
  }
  return std::nullopt;
}

}  // namespace

std::string_view RoomKindName(RoomKind kind) {
  switch (kind) {
    case RoomKind::kSpace:
      return "space";
    case RoomKind::kFolder:
      return "folder";
    case RoomKind::kEasel:
      return "easel";
  }
  NOTREACHED();
}

std::optional<RoomKind> RoomKindFromName(std::string_view name) {
  for (RoomKind kind :
       {RoomKind::kSpace, RoomKind::kFolder, RoomKind::kEasel}) {
    if (RoomKindName(kind) == name) {
      return kind;
    }
  }
  return std::nullopt;
}

std::optional<std::string> InviteLinkJson(const InviteLink& link) {
  if (!IsValid(link)) {
    return std::nullopt;
  }
  std::string json =
      base::StrCat({"{\"e\":", base::NumberToString(link.epoch), ",\"i\":"});
  AppendJsonString(json, link.invite_id);
  json += ",\"k\":";
  AppendJsonString(json, B64(link.epoch_key));
  json += ",\"n\":";
  AppendJsonString(json, link.name);
  json += ",\"r\":";
  AppendJsonString(json, B64(link.room_id));
  json += ",\"s\":";
  AppendJsonString(json, link.server_url);
  json += ",\"t\":";
  AppendJsonString(json, RoomKindName(link.kind));
  json += ",\"x\":";
  AppendJsonString(json, B64(link.invite_secret));
  json += "}";
  return json;
}

std::optional<std::string> EncodeInviteLink(const InviteLink& link) {
  std::optional<std::string> json = InviteLinkJson(link);
  if (!json) {
    return std::nullopt;
  }
  std::string out =
      base::StrCat({kInviteLinkPrefix, B64(base::as_byte_span(*json))});
  if (out.size() > kMaxInviteLinkSize) {
    return std::nullopt;
  }
  return out;
}

std::optional<InviteLink> ParseInviteLink(std::string_view text) {
  if (text.size() > kMaxInviteLinkSize) {
    return std::nullopt;
  }
  std::string_view fragment;
  if (text.starts_with(kInviteLinkPrefix)) {
    fragment = text.substr(std::string_view(kInviteLinkPrefix).size());
  } else if (text.starts_with(kInviteDeepLinkPrefix)) {
    fragment = text.substr(std::string_view(kInviteDeepLinkPrefix).size());
  } else {
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> json = base::Base64UrlDecode(
      fragment, base::Base64UrlDecodePolicy::DISALLOW_PADDING);
  if (!json || json->empty() || B64(*json) != fragment) {
    return std::nullopt;
  }
  std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      base::as_string_view(*json), base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }

  const std::string* server_url = dict->FindString("s");
  const std::string* invite_id = dict->FindString("i");
  const std::string* name = dict->FindString("n");
  const std::string* kind_name = dict->FindString("t");
  auto room_id = FromB64<kRoomIdSize>(dict->FindString("r"));
  auto secret = FromB64<kInviteSecretSize>(dict->FindString("x"));
  auto key = FromB64<kRoomKeySize>(dict->FindString("k"));
  std::optional<uint32_t> epoch = GetEpoch(dict->Find("e"));
  if (!server_url || !invite_id || !name || !kind_name || !room_id || !secret ||
      !key || !epoch) {
    return std::nullopt;
  }
  std::optional<RoomKind> kind = RoomKindFromName(*kind_name);
  if (!kind) {
    return std::nullopt;
  }
  InviteLink link;
  link.server_url = *server_url;
  link.room_id = *room_id;
  link.invite_id = *invite_id;
  link.invite_secret = *secret;
  link.epoch_key = *key;
  link.epoch = *epoch;
  link.name = *name;
  link.kind = *kind;
  if (!IsValid(link)) {
    return std::nullopt;
  }
  return link;
}

}  // namespace compass::sync::rooms
