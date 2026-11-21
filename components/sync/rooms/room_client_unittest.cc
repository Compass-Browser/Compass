// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/rooms/room_client.h"

#include <optional>
#include <string>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::sync::rooms {
namespace {

constexpr char kServer[] = "https://sync.example/v2";
constexpr char kRoom[] = "n4bQgYhMfWWaL-qgxVrQFQ";

class RoomClientTest : public testing::Test {
 protected:
  RoomClientTest()
      : client_(GURL(kServer),
                base::MakeRefCounted<
                    network::WeakWrapperSharedURLLoaderFactory>(&factory_),
                base::BindRepeating(
                    []() -> std::optional<std::string> { return "TOKEN"; }),
                base::BindRepeating([] { return std::string("guid/1"); })) {
    factory_.SetInterceptor(
        base::BindLambdaForTesting([this](const network::ResourceRequest& r) {
          last_ = r;
        }));
  }

  std::string LastBody() {
    return std::string(
        last_.request_body->elements()->at(0).As<network::DataElementBytes>()
            .AsStringPiece());
  }

  base::test::TaskEnvironment task_environment_;
  network::TestURLLoaderFactory factory_;
  network::ResourceRequest last_;
  RoomClient client_;
};

TEST_F(RoomClientTest, MakeUrl) {
  EXPECT_EQ(RoomClient::MakeUrl(GURL("https://s.example/v2/"), "rooms/x/changes",
                                "since=4", "a+b")
                .spec(),
            "https://s.example/v2/compass/rooms/x/changes?since=4&client_id=a%2Bb");
  EXPECT_EQ(RoomClient::MakeUrl(GURL("http://localhost:8295/v2"), "identity",
                                {}, {})
                .spec(),
            "http://localhost:8295/v2/compass/identity");
}

TEST_F(RoomClientTest, GetRoomParsesEverything) {
  const std::string pub = base::Base64Encode(std::string(32, 'p'));
  const std::string sealed = base::Base64Encode(std::string(80, 's'));
  factory_.AddResponse(
      std::string(kServer) + "/compass/rooms/" + kRoom + "?client_id=guid%2F1",
      R"({"id":")" + std::string(kRoom) +
          R"(","kind":"folder","owner":"aa","created":17,"epoch":2,"seq":42,
          "bytes":99,"role":"owner","pending":false,
          "members":[{"client_id":"aa","role":"owner","identity_pub":")" +
          pub + R"(","profile":null,"name":"Ada","email":"ada@x","invited":1,
          "joined":2,"pending":false},{"client_id":"bb","role":"viewer",
          "identity_pub":")" +
          pub + R"(","pending":true}],
          "envelopes":[{"epoch":1,"sealed":")" +
          sealed + R"("},{"epoch":2,"sealed":")" + sealed + R"("}],
          "invites":[{"id":"inv","role":"editor","created":3,"expires":0,
          "uses_left":null,"revoked":false}]})");
  base::test::TestFuture<RoomResult<RoomInfo>> future;
  client_.GetRoom(kRoom, future.GetCallback());
  const RoomResult<RoomInfo>& result = future.Get();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(last_.headers.GetHeader("Authorization"), "Bearer TOKEN");
  EXPECT_EQ(result->kind, "folder");
  EXPECT_EQ(result->epoch, 2u);
  EXPECT_EQ(result->seq, 42);
  ASSERT_EQ(result->members.size(), 2u);
  EXPECT_EQ(result->members[0].name, "Ada");
  EXPECT_EQ(result->members[0].identity_pub[0], 'p');
  EXPECT_TRUE(result->members[1].pending);
  ASSERT_EQ(result->envelopes.size(), 2u);
  EXPECT_EQ(result->envelopes[1].epoch, 2u);
  EXPECT_EQ(result->envelopes[1].sealed.size(), 80u);
  ASSERT_EQ(result->invites.size(), 1u);
  EXPECT_FALSE(result->invites[0].uses_left);
  EXPECT_TRUE(result->FindMember("BB"));
}

TEST_F(RoomClientTest, ChangesAndLongPoll) {
  factory_.AddResponse(
      std::string(kServer) + "/compass/rooms/" + kRoom +
          "/changes?since=5&wait=30&client_id=guid%2F1",
      R"({"seq":7,"epoch":1,"more":true,"records":[
        {"id":"node:a","seq":6,"epoch":1,"deleted":false,"author":"aa",
         "ciphertext":"AAEC"},
        {"id":"node:b","seq":7,"epoch":1,"deleted":true,"author":"aa"}]})");
  base::test::TestFuture<RoomResult<RoomChanges>> future;
  client_.GetChanges(kRoom, 5, 99, future.GetCallback());
  ASSERT_TRUE(future.Get().has_value());
  const RoomChanges& changes = *future.Get();
  EXPECT_EQ(changes.seq, 7);
  EXPECT_TRUE(changes.more);
  ASSERT_EQ(changes.records.size(), 2u);
  EXPECT_EQ(changes.records[0].ciphertext, (std::vector<uint8_t>{0, 1, 2}));
  EXPECT_TRUE(changes.records[1].deleted);
  EXPECT_TRUE(changes.records[1].ciphertext.empty());
}

TEST_F(RoomClientTest, CommitSendsBaseSeqAndMapsConflict) {
  factory_.AddResponse(
      std::string(kServer) + "/compass/rooms/" + kRoom +
          "/records?client_id=guid%2F1",
      R"({"error":"conflict","current":[{"id":"x","seq":9,"epoch":1,
          "deleted":false,"author":"bb","ciphertext":"AAAA"},
          {"id":"y","seq":0,"epoch":0,"deleted":false,"author":""}]})",
      net::HTTP_CONFLICT);
  std::vector<RecordWrite> writes(2);
  writes[0].id = "x";
  writes[0].epoch = 1;
  writes[0].ciphertext = {1, 2, 3};
  writes[0].base_seq = 4;
  writes[1].id = "y";
  writes[1].epoch = 1;
  writes[1].deleted = true;
  writes[1].base_seq = 0;
  base::test::TestFuture<RoomResult<CommitResult>> future;
  client_.CommitRecords(kRoom, writes, future.GetCallback());
  ASSERT_FALSE(future.Get().has_value());
  const RoomError& error = future.Get().error();
  EXPECT_TRUE(error.is_conflict());
  ASSERT_EQ(error.current.size(), 2u);
  EXPECT_EQ(error.current[0].seq, 9);
  EXPECT_EQ(error.current[1].seq, 0);

  std::optional<base::Value> body = base::JSONReader::Read(LastBody(), base::JSON_PARSE_RFC);
  ASSERT_TRUE(body && body->is_dict());
  const base::ListValue* records = body->GetDict().FindList("records");
  ASSERT_TRUE(records);
  EXPECT_EQ(records->front().GetDict().FindDouble("base_seq"), 4);
  EXPECT_EQ(*records->front().GetDict().FindString("ciphertext"), "AQID");
  EXPECT_EQ((*records)[1].GetDict().FindBool("deleted"), true);
  EXPECT_FALSE((*records)[1].GetDict().FindString("ciphertext"));
  EXPECT_EQ((*records)[1].GetDict().FindDouble("base_seq"), 0);
  // Integers on the wire, not "4.0": the server's fields are int64s.
  EXPECT_NE(LastBody().find("\"base_seq\":4,"), std::string::npos)
      << LastBody();
}

TEST_F(RoomClientTest, ErrorCodes) {
  struct {
    int status;
    const char* body;
    bool (RoomError::*check)() const;
  } cases[] = {
      {403, R"({"error":"removed"})", &RoomError::is_gone},
      {404, R"({"error":"not_found"})", &RoomError::is_gone},
      {403, R"({"error":"forbidden"})", &RoomError::is_forbidden},
      {403, R"({"error":"chain_not_allowed"})",
       &RoomError::is_chain_not_allowed},
      {409, R"({"error":"stale_epoch"})", &RoomError::is_stale_epoch},
      {413, R"({"error":"record_too_large"})", &RoomError::is_too_large},
      {507, R"({"error":"quota_exceeded"})", &RoomError::is_quota_exceeded},
      {401, R"({"error":"login_required"})", &RoomError::is_login_required},
  };
  for (const auto& c : cases) {
    const RoomError error = RoomClient::ParseError(c.status, c.body);
    EXPECT_TRUE((error.*c.check)()) << c.body;
  }
  const RoomError mismatch = RoomClient::ParseError(
      409, R"({"error":"envelopes_mismatch","missing":["a"],"unknown":["b"]})");
  EXPECT_EQ(mismatch.missing, std::vector<std::string>{"a"});
  EXPECT_EQ(mismatch.unknown, std::vector<std::string>{"b"});
}

TEST_F(RoomClientTest, NetworkErrorIsStatusZero) {
  factory_.AddResponse(
      GURL(std::string(kServer) + "/compass/rooms?client_id=guid%2F1"),
      network::mojom::URLResponseHead::New(), "",
      network::URLLoaderCompletionStatus(net::ERR_CONNECTION_REFUSED));
  base::test::TestFuture<RoomResult<std::vector<RoomSummary>>> future;
  client_.ListRooms(future.GetCallback());
  ASSERT_FALSE(future.Get().has_value());
  EXPECT_TRUE(future.Get().error().is_network());
}

TEST_F(RoomClientTest, SuccessorSendsLinkedToken) {
  factory_.AddResponse(std::string(kServer) + "/compass/rooms/" + kRoom +
                           "/successor?client_id=guid%2F1",
                       "", net::HTTP_NO_CONTENT);
  base::test::TestFuture<std::optional<RoomError>> future;
  client_.MoveToSuccessor(kRoom, "NEWTOKEN", X25519Key{},
                          {{1, std::vector<uint8_t>(80, 1)}},
                          future.GetCallback());
  EXPECT_FALSE(future.Get());
  EXPECT_EQ(last_.headers.GetHeader("X-Compass-Linked-Token"), "NEWTOKEN");
  EXPECT_EQ(last_.method, "POST");
}

TEST_F(RoomClientTest, InviteAndJoinBodies) {
  factory_.AddResponse(
      std::string(kServer) + "/compass/rooms/" + kRoom +
          "/invites?client_id=guid%2F1",
      R"({"id":"inv1","role":"viewer","created":1,"expires":5,
          "uses_left":3,"revoked":false})",
      net::HTTP_CREATED);
  base::test::TestFuture<RoomResult<RoomInvite>> invite;
  client_.CreateInvite(kRoom, "viewer", "ab12", 5, 3, invite.GetCallback());
  ASSERT_TRUE(invite.Get().has_value());
  EXPECT_EQ(invite.Get()->uses_left, 3);
  std::optional<base::Value> body = base::JSONReader::Read(LastBody(), base::JSON_PARSE_RFC);
  EXPECT_EQ(*body->GetDict().FindString("secret_hash"), "ab12");
  EXPECT_EQ(body->GetDict().FindInt("uses"), 3);
}

}  // namespace
}  // namespace compass::sync::rooms
