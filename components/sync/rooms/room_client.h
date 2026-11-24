// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_CLIENT_H_
#define COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_CLIENT_H_

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "compass/components/sync/rooms/room_crypto.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

// The rooms API of a Compass sync server (docs/design/sync.md, "Rooms"):
// every route, signed with the account's chain token, JSON in and out.
// Binary fields are decoded here (standard base64), room and invite IDs and
// join secrets stay unpadded base64url strings, as the server writes them.
// iOS has its own (CompassKit's CompassRooms).
namespace compass::sync::rooms {

// Roles, as the server names them.
inline constexpr char kRoleOwner[] = "owner";
inline constexpr char kRoleEditor[] = "editor";
inline constexpr char kRoleViewer[] = "viewer";

struct RoomMember {
  RoomMember();
  RoomMember(const RoomMember&);
  RoomMember& operator=(const RoomMember&);
  ~RoomMember();

  // The chain ID, lowercase hex.
  std::string client_id;
  std::string role;
  X25519Key identity_pub{};
  // The member's own blob (an encrypted RoomProfile), empty if none.
  std::vector<uint8_t> profile;
  // From company sign-in; empty without.
  std::string name;
  std::string email;
  int64_t invited = 0;
  int64_t joined = 0;
  bool pending = false;
};

struct RoomInvite {
  std::string id;
  std::string role;
  int64_t created = 0;
  int64_t expires = 0;  // 0: never.
  std::optional<int> uses_left;  // nullopt: unlimited.
  bool revoked = false;
};

struct RoomEnvelope {
  uint32_t epoch = 0;
  std::vector<uint8_t> sealed;
};

// A room as GET /rooms/{room} answers it.
struct RoomInfo {
  RoomInfo();
  RoomInfo(const RoomInfo&);
  RoomInfo& operator=(const RoomInfo&);
  RoomInfo(RoomInfo&&);
  RoomInfo& operator=(RoomInfo&&);
  ~RoomInfo();

  const RoomMember* FindMember(std::string_view client_id) const;

  std::string id;
  std::string kind;
  std::string owner;
  int64_t created = 0;
  uint32_t epoch = 0;
  int64_t seq = 0;
  int64_t bytes = 0;
  // The caller's.
  std::string role;
  bool pending = false;
  std::vector<RoomMember> members;
  // The caller's own, by epoch.
  std::vector<RoomEnvelope> envelopes;
  // The owner's answers only.
  std::vector<RoomInvite> invites;
};

// A room in GET /rooms.
struct RoomSummary {
  std::string id;
  std::string kind;
  std::string owner;
  std::string role;
  bool pending = false;
  uint32_t epoch = 0;
  int64_t seq = 0;
};

struct RoomRecord {
  RoomRecord();
  RoomRecord(const RoomRecord&);
  RoomRecord& operator=(const RoomRecord&);
  RoomRecord(RoomRecord&&);
  RoomRecord& operator=(RoomRecord&&);
  ~RoomRecord();

  std::string id;
  int64_t seq = 0;
  uint32_t epoch = 0;
  bool deleted = false;
  std::string author;
  // nonce || ciphertext || tag; empty when deleted.
  std::vector<uint8_t> ciphertext;
};

struct RoomChanges {
  RoomChanges();
  RoomChanges(RoomChanges&&);
  RoomChanges& operator=(RoomChanges&&);
  ~RoomChanges();

  int64_t seq = 0;
  uint32_t epoch = 0;
  std::vector<RoomRecord> records;
  bool more = false;
};

// One write of POST /rooms/{room}/records.
struct RecordWrite {
  RecordWrite();
  RecordWrite(const RecordWrite&);
  RecordWrite& operator=(const RecordWrite&);
  RecordWrite(RecordWrite&&);
  RecordWrite& operator=(RecordWrite&&);
  ~RecordWrite();

  std::string id;
  uint32_t epoch = 0;
  std::vector<uint8_t> ciphertext;
  bool deleted = false;
  // The seq the write is based on; 0: the record must not exist.
  std::optional<int64_t> base_seq;
};

struct CommitResult {
  CommitResult();
  CommitResult(CommitResult&&);
  CommitResult& operator=(CommitResult&&);
  ~CommitResult();

  int64_t seq = 0;
  // The seq of each record written, by ID.
  std::map<std::string, int64_t> records;
};

struct DirectoryAccount {
  std::string client_id;
  X25519Key identity_pub{};
  std::string name;
  std::string email;
};

// What /v2/compass/info says: whether company sign-in (OIDC) is on, so the
// directory and email invites work.
struct ServerInfo {
  bool oidc = false;
  std::string login_name;
};

// A request that failed. |status| 0: no answer (network, timeout, a reply
// that isn't JSON where one was due).
struct RoomError {
  RoomError();
  RoomError(int status, std::string code);
  RoomError(const RoomError&);
  RoomError& operator=(const RoomError&);
  RoomError(RoomError&&);
  RoomError& operator=(RoomError&&);
  ~RoomError();

  bool is_network() const { return status == 0; }
  // 401 login_required: the server wants company sign-in first.
  bool is_login_required() const { return status == 401 && code == "login_required"; }
  // 403 removed (left, or removed by the owner), or 404 not_found (the room
  // was deleted, or never had the caller).
  bool is_gone() const {
    return (status == 403 && code == "removed") ||
           (status == 404 && code == "not_found");
  }
  bool is_forbidden() const { return status == 403 && code == "forbidden"; }
  bool is_conflict() const { return status == 409 && code == "conflict"; }
  bool is_stale_epoch() const {
    return status == 409 && code == "stale_epoch";
  }
  bool is_too_large() const { return status == 413; }
  bool is_quota_exceeded() const { return status == 507; }
  bool is_chain_not_allowed() const {
    return status == 403 && code == "chain_not_allowed";
  }

  int status = 0;
  std::string code;
  // 409 conflict: the stored state of each mismatched record (seq 0 and no
  // ciphertext: it doesn't exist).
  std::vector<RoomRecord> current;
  // 409 envelopes_mismatch.
  std::vector<std::string> missing;
  std::vector<std::string> unknown;
};

template <typename T>
using RoomResult = base::expected<T, RoomError>;

class RoomClient {
 public:
  // A fresh chain token, or nullopt without a key.
  using TokenGetter = base::RepeatingCallback<std::optional<std::string>()>;
  // This device's sync cache GUID, sent as client_id so its own changes'
  // events skip it; empty if not known.
  using ClientIdGetter = base::RepeatingCallback<std::string()>;
  template <typename T>
  using Callback = base::OnceCallback<void(RoomResult<T>)>;
  using DoneCallback = base::OnceCallback<void(std::optional<RoomError>)>;

  // Longest long poll the server holds (s).
  static constexpr int kMaxWaitSeconds = 30;

  // |server_url| is the sync server's (its path ends where /compass/...
  // starts, e.g. https://sync.example/v2).
  RoomClient(const GURL& server_url,
             scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
             TokenGetter token_getter,
             ClientIdGetter client_id_getter);
  RoomClient(const RoomClient&) = delete;
  RoomClient& operator=(const RoomClient&) = delete;
  ~RoomClient();

  const GURL& server_url() const { return server_url_; }

  // <server path>/compass/<path>, with ?client_id= and |query|.
  static GURL MakeUrl(const GURL& server_url,
                      std::string_view path,
                      std::string_view query,
                      std::string_view client_id);

  void GetInfo(Callback<ServerInfo> callback);
  void ListRooms(Callback<std::vector<RoomSummary>> callback);
  // |room_id|: the ID the client chose (unpadded base64url of 16 bytes), so
  // |envelope| can be sealed for it; empty lets the server pick one.
  void CreateRoom(std::string_view room_id,
                  std::string_view kind,
                  const X25519Key& identity_pub,
                  const std::vector<uint8_t>& envelope,
                  const std::vector<uint8_t>& profile,
                  Callback<RoomInfo> callback);
  void GetRoom(std::string_view room_id, Callback<RoomInfo> callback);
  void DeleteRoom(std::string_view room_id, DoneCallback callback);
  // |wait| up to kMaxWaitSeconds: a long poll.
  void GetChanges(std::string_view room_id,
                  int64_t since,
                  int wait_seconds,
                  Callback<RoomChanges> callback);
  void CommitRecords(std::string_view room_id,
                     const std::vector<RecordWrite>& writes,
                     Callback<CommitResult> callback);
  // |expires| Unix ms, 0 never; |uses| 0 unlimited.
  void CreateInvite(std::string_view room_id,
                    std::string_view role,
                    std::string_view secret_hash,
                    int64_t expires,
                    int uses,
                    Callback<RoomInvite> callback);
  void RevokeInvite(std::string_view room_id,
                    std::string_view invite_id,
                    DoneCallback callback);
  // |invite_id| and |secret| (the link's i and x, unpadded base64url) may be
  // empty for a pending member.
  void Join(std::string_view room_id,
            std::string_view invite_id,
            std::string_view secret,
            const X25519Key& identity_pub,
            const std::vector<uint8_t>& profile,
            Callback<RoomInfo> callback);
  // Email invites (company sign-in): each member with its envelopes.
  struct NewMember {
    NewMember();
    NewMember(const NewMember&);
    NewMember& operator=(const NewMember&);
    ~NewMember();
    std::string client_id;
    std::string role;
    X25519Key identity_pub{};
    std::vector<RoomEnvelope> envelopes;
  };
  void AddMembers(std::string_view room_id,
                  const std::vector<NewMember>& members,
                  Callback<RoomInfo> callback);
  // |role| (owner, on another member) or |profile| (oneself; empty clears).
  void UpdateMember(std::string_view room_id,
                    std::string_view client_id,
                    std::optional<std::string> role,
                    std::optional<std::vector<uint8_t>> profile,
                    DoneCallback callback);
  void RemoveMember(std::string_view room_id,
                    std::string_view client_id,
                    DoneCallback callback);
  void TransferOwner(std::string_view room_id,
                     std::string_view client_id,
                     DoneCallback callback);
  // Moves the caller's membership to the chain |linked_token| signs for
  // (Reset sync), with that chain's identity key and every epoch's key
  // sealed to it.
  void MoveToSuccessor(std::string_view room_id,
                       std::string_view linked_token,
                       const X25519Key& identity_pub,
                       const std::vector<RoomEnvelope>& envelopes,
                       DoneCallback callback);
  // |envelopes|: client ID -> sealed key of |epoch|.
  void Rotate(std::string_view room_id,
              uint32_t epoch,
              const std::map<std::string, std::vector<uint8_t>>& envelopes,
              DoneCallback callback);
  struct MemberEnvelope {
    std::string client_id;
    uint32_t epoch = 0;
    std::vector<uint8_t> sealed;
  };
  void AddEnvelopes(std::string_view room_id,
                    const std::vector<MemberEnvelope>& envelopes,
                    DoneCallback callback);
  void GetIdentity(Callback<std::optional<X25519Key>> callback);
  void PutIdentity(const X25519Key& identity_pub, DoneCallback callback);
  void LookUpDirectory(std::string_view email,
                       Callback<std::vector<DirectoryAccount>> callback);

  // Parsers, exposed for tests.
  static std::optional<RoomInfo> ParseRoom(const base::DictValue& dict);
  static std::optional<RoomRecord> ParseRecord(const base::DictValue& dict);
  static RoomError ParseError(int status, const std::string& body);

  size_t pending_requests_for_testing() const { return loaders_.size(); }

 private:
  // The answer: status (0 none) and body.
  using RawCallback =
      base::OnceCallback<void(int status, std::optional<std::string> body)>;
  // A request in flight, and who gets its answer.
  struct Pending {
    std::unique_ptr<network::SimpleURLLoader> loader;
    RawCallback callback;
  };
  using Loaders = std::list<Pending>;

  void Send(std::string_view method,
            std::string_view path,
            std::string_view query,
            const base::DictValue* body,
            base::TimeDelta timeout,
            RawCallback callback,
            std::optional<std::string> linked_token = std::nullopt);
  void OnResponse(Loaders::iterator pending, std::optional<std::string> body);
  // Ends a reply larger than kMaxReplySize as a network error.
  void OnProgress(Loaders::iterator pending, uint64_t current);
  // Calls Send() and parses a JSON object with |parse|.
  template <typename T>
  void SendForJson(std::string_view method,
                   std::string_view path,
                   std::string_view query,
                   const base::DictValue* body,
                   base::TimeDelta timeout,
                   base::OnceCallback<std::optional<T>(const base::DictValue&)>
                       parse,
                   Callback<T> callback);
  void SendForDone(std::string_view method,
                   std::string_view path,
                   const base::DictValue* body,
                   DoneCallback callback,
                   std::optional<std::string> linked_token = std::nullopt);

  const GURL server_url_;
  scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  TokenGetter token_getter_;
  ClientIdGetter client_id_getter_;
  Loaders loaders_;
  base::WeakPtrFactory<RoomClient> weak_factory_{this};
};

}  // namespace compass::sync::rooms

#endif  // COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_CLIENT_H_
