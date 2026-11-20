// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/rooms/room_invite_link.h"

#include "base/strings/strcat.h"
#include "compass/components/sync/rooms/room_test_vectors.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::sync::rooms {
namespace {

using test::Array;
using test::Epoch;
using test::Section;
using test::Str;

InviteLink Expected(const base::DictValue& v) {
  const base::DictValue* e = v.FindDict("expected");
  EXPECT_TRUE(e);
  if (!e) {
    return {};
  }
  InviteLink link;
  link.server_url = Str(*e, "server_url");
  link.room_id = Array<kRoomIdSize>(*e, "room_id");
  link.invite_id = Str(*e, "invite_id");
  link.invite_secret = Array<kInviteSecretSize>(*e, "invite_secret");
  link.epoch_key = Array<kRoomKeySize>(*e, "epoch_key");
  link.epoch = Epoch(*e);
  link.name = Str(*e, "name");
  link.kind = RoomKindFromName(Str(*e, "kind")).value_or(RoomKind::kSpace);
  return link;
}

TEST(RoomInviteLinkTest, VectorsParse) {
  for (const base::DictValue& v : Section("links")) {
    SCOPED_TRACE(Str(v, "name"));
    const std::optional<InviteLink> link = ParseInviteLink(Str(v, "link"));
    ASSERT_TRUE(link);
    EXPECT_EQ(*link, Expected(v));
  }
}

TEST(RoomInviteLinkTest, VectorsEncode) {
  for (const base::DictValue& v : Section("links")) {
    if (!v.FindBool("encodes").value_or(false)) {
      continue;
    }
    SCOPED_TRACE(Str(v, "name"));
    EXPECT_EQ(InviteLinkJson(Expected(v)), Str(v, "json"));
    EXPECT_EQ(EncodeInviteLink(Expected(v)), Str(v, "link"));
  }
}

TEST(RoomInviteLinkTest, InvalidVectors) {
  for (const base::DictValue& v : Section("invalid_links")) {
    SCOPED_TRACE(Str(v, "name"));
    EXPECT_FALSE(ParseInviteLink(Str(v, "link")));
  }
}

TEST(RoomInviteLinkTest, RoundTripRandom) {
  InviteLink link;
  link.server_url = "https://sync.example.com/v2";
  link.room_id = GenerateRoomId();
  link.invite_id = "abc";
  link.invite_secret = GenerateInviteSecret();
  link.epoch_key = GenerateRoomKey();
  link.epoch = 42;
  link.name = "Board \"🧭\"";
  link.kind = RoomKind::kEasel;
  const std::optional<std::string> text = EncodeInviteLink(link);
  ASSERT_TRUE(text);
  EXPECT_EQ(ParseInviteLink(*text), link);
  // The deep link carries the same fragment.
  EXPECT_EQ(ParseInviteLink(base::StrCat(
                {kInviteDeepLinkPrefix,
                 text->substr(std::string_view(kInviteLinkPrefix).size())})),
            link);
}

TEST(RoomInviteLinkTest, EncodeRefusesInvalid) {
  InviteLink link;
  link.server_url = "http://sync.example.com";
  link.invite_id = "abc";
  EXPECT_FALSE(EncodeInviteLink(link));
  link.server_url = "https://sync.example.com";
  EXPECT_TRUE(EncodeInviteLink(link));
  link.invite_id = "";
  EXPECT_FALSE(EncodeInviteLink(link));
  link.invite_id = "abc";
  link.name = std::string(kMaxRoomNameSize + 1, 'a');
  EXPECT_FALSE(EncodeInviteLink(link));
  link.name = "\xff";
  EXPECT_FALSE(EncodeInviteLink(link));
}

TEST(RoomInviteLinkTest, KindNames) {
  for (RoomKind kind :
       {RoomKind::kSpace, RoomKind::kFolder, RoomKind::kEasel}) {
    EXPECT_EQ(RoomKindFromName(RoomKindName(kind)), kind);
  }
  EXPECT_FALSE(RoomKindFromName("tab"));
}

}  // namespace
}  // namespace compass::sync::rooms
