// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_INVITE_LINK_H_
#define COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_INVITE_LINK_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "compass/components/sync/rooms/room_crypto.h"

// Invite links: everything a new member needs to join a room, in the
// fragment of https://compassbrowser.org/join#... (or compass://join#...),
// which never reaches a server. See docs/design/sync.md, "Rooms: crypto and
// links", and common/rooms/vectors.json. The iOS twin is
// ios/CompassKit/Sources/CompassRooms/RoomInviteLink.swift.
namespace compass::sync::rooms {

inline constexpr char kInviteLinkPrefix[] = "https://compassbrowser.org/join#";
inline constexpr char kInviteDeepLinkPrefix[] = "compass://join#";

// Limits a link must keep to.
inline constexpr size_t kMaxInviteLinkSize = 4096;
inline constexpr size_t kMaxInviteIdSize = 64;
inline constexpr size_t kMaxRoomNameSize = 256;  // UTF-8 bytes

enum class RoomKind { kSpace, kFolder, kEasel };

// "space", "folder" or "easel".
std::string_view RoomKindName(RoomKind kind);
std::optional<RoomKind> RoomKindFromName(std::string_view name);

struct InviteLink {
  // The room's server: https, or http on localhost, 127.0.0.1 or [::1]. No
  // user info, query or fragment.
  std::string server_url;
  RoomId room_id{};
  // The server's ID for the invite: 1 to 64 of [A-Za-z0-9_-].
  std::string invite_id;
  // Proves the invite to the server, which keeps InviteSecretHash() of it.
  InviteSecret invite_secret{};
  // The room key of |epoch|, so the joiner reads at once.
  RoomKey epoch_key{};
  uint32_t epoch = 0;
  // The shared item's name, for the join preview. At most 256 UTF-8 bytes.
  std::string name;
  RoomKind kind = RoomKind::kSpace;

  bool operator==(const InviteLink&) const = default;
};

// The link's JSON, compact with sorted keys:
// {"e":epoch,"i":invite_id,"k":b64url(epoch_key),"n":name,
//  "r":b64url(room_id),"s":server_url,"t":kind,"x":b64url(invite_secret)}
// Strings escape only '"', '\' and control characters; base64url has no
// padding. Nullopt if |link| breaks a rule above.
std::optional<std::string> InviteLinkJson(const InviteLink& link);

// kInviteLinkPrefix + base64url(InviteLinkJson()), or nullopt.
std::optional<std::string> EncodeInviteLink(const InviteLink& link);

// The invite in an https://compassbrowser.org/join# or compass://join# link,
// or nullopt if it's malformed or breaks a rule above. Unknown JSON keys are
// ignored, for later versions.
std::optional<InviteLink> ParseInviteLink(std::string_view text);

}  // namespace compass::sync::rooms

#endif  // COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_INVITE_LINK_H_
