// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/rooms/room_client.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync::rooms {

namespace {

// Changes pages are about 8 MiB of ciphertext, base64 a third more.
constexpr size_t kMaxReplySize = 16 * 1024 * 1024;
constexpr base::TimeDelta kRequestTimeout = base::Seconds(30);
// A long poll's answer comes within its wait; this is the slack.
constexpr base::TimeDelta kLongPollSlack = base::Seconds(15);
constexpr char kLinkedTokenHeader[] = "X-Compass-Linked-Token";

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_rooms", R"(
        semantics {
          sender: "Compass sharing: rooms"
          description:
            "Shares spaces, pinned folders and easels with other people "
            "through a Compass sync server: creates rooms, invites and "
            "members, and reads and writes the shared items' records, which "
            "are end-to-end encrypted: the server cannot read them."
          trigger: "The user shares an item, joins one from an invite link, "
                   "or changes a shared item; and while shared items are "
                   "open, to receive other members' changes."
          data: "The chain's signed token, this device's sync ID, encrypted "
                "records, sealed room keys, the account's public identity "
                "key, and invite IDs and secrets."
          destination: OTHER
          destination_other: "The self-hosted sync server of the room."
        }
        policy {
          cookies_allowed: NO
          setting: "Only while the user shares or has joined shared items."
          policy_exception_justification: "Not implemented."
        })");

std::string B64(base::span<const uint8_t> bytes) {
  return base::Base64Encode(bytes);
}

std::optional<std::vector<uint8_t>> FromB64(const std::string* text) {
  if (!text) {
    return std::nullopt;
  }
  return base::Base64Decode(*text);
}

std::optional<X25519Key> KeyFromB64(const std::string* text) {
  std::optional<std::vector<uint8_t>> bytes = FromB64(text);
  if (!bytes || bytes->size() != kX25519KeySize) {
    return std::nullopt;
  }
  X25519Key key;
  std::ranges::copy(*bytes, key.begin());
  return key;
}

int64_t Int(const base::DictValue& dict, std::string_view key) {
  if (std::optional<double> value = dict.FindDouble(key)) {
    return static_cast<int64_t>(*value);
  }
  return 0;
}

std::string Str(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

std::optional<RoomMember> ParseMember(const base::DictValue& dict) {
  RoomMember member;
  member.client_id = Str(dict, "client_id");
  member.role = Str(dict, "role");
  if (member.client_id.empty() || member.role.empty()) {
    return std::nullopt;
  }
  if (std::optional<X25519Key> key =
          KeyFromB64(dict.FindString("identity_pub"))) {
    member.identity_pub = *key;
  }
  if (std::optional<std::vector<uint8_t>> profile =
          FromB64(dict.FindString("profile"))) {
    member.profile = std::move(*profile);
  }
  member.name = Str(dict, "name");
  member.email = Str(dict, "email");
  member.invited = Int(dict, "invited");
  member.joined = Int(dict, "joined");
  member.pending = dict.FindBool("pending").value_or(false);
  return member;
}

std::optional<RoomInvite> ParseInvite(const base::DictValue& dict) {
  RoomInvite invite;
  invite.id = Str(dict, "id");
  invite.role = Str(dict, "role");
  if (invite.id.empty()) {
    return std::nullopt;
  }
  invite.created = Int(dict, "created");
  invite.expires = Int(dict, "expires");
  if (std::optional<int> uses = dict.FindInt("uses_left")) {
    invite.uses_left = *uses;
  }
  invite.revoked = dict.FindBool("revoked").value_or(false);
  return invite;
}

std::optional<RoomChanges> ParseChanges(const base::DictValue& dict) {
  RoomChanges changes;
  changes.seq = Int(dict, "seq");
  changes.epoch = static_cast<uint32_t>(Int(dict, "epoch"));
  changes.more = dict.FindBool("more").value_or(false);
  if (const base::ListValue* records = dict.FindList("records")) {
    for (const base::Value& value : *records) {
      if (!value.is_dict()) {
        return std::nullopt;
      }
      std::optional<RoomRecord> record =
          RoomClient::ParseRecord(value.GetDict());
      if (!record) {
        return std::nullopt;
      }
      changes.records.push_back(std::move(*record));
    }
  }
  return changes;
}

std::optional<RoomInvite> ParseInviteAnswer(const base::DictValue& dict) {
  return ParseInvite(dict);
}

std::optional<ServerInfo> ParseInfo(const base::DictValue& dict) {
  ServerInfo info;
  if (const base::DictValue* login = dict.FindDict("login")) {
    info.oidc = Str(*login, "type") == "oidc";
    info.login_name = Str(*login, "name");
  }
  return info;
}

std::optional<std::vector<RoomSummary>> ParseRoomList(
    const base::DictValue& dict) {
  std::vector<RoomSummary> rooms;
  if (const base::ListValue* list = dict.FindList("rooms")) {
    for (const base::Value& value : *list) {
      if (!value.is_dict()) {
        continue;
      }
      const base::DictValue& room = value.GetDict();
      RoomSummary summary;
      summary.id = Str(room, "id");
      summary.kind = Str(room, "kind");
      summary.owner = Str(room, "owner");
      summary.role = Str(room, "role");
      summary.pending = room.FindBool("pending").value_or(false);
      summary.epoch = static_cast<uint32_t>(Int(room, "epoch"));
      summary.seq = Int(room, "seq");
      if (!summary.id.empty()) {
        rooms.push_back(std::move(summary));
      }
    }
  }
  return rooms;
}

std::optional<CommitResult> ParseCommit(const base::DictValue& dict) {
  CommitResult result;
  result.seq = Int(dict, "seq");
  if (const base::ListValue* list = dict.FindList("records")) {
    for (const base::Value& value : *list) {
      if (value.is_dict()) {
        result.records[Str(value.GetDict(), "id")] =
            Int(value.GetDict(), "seq");
      }
    }
  }
  return result;
}

std::optional<std::optional<X25519Key>> ParseIdentity(
    const base::DictValue& dict) {
  return std::optional<std::optional<X25519Key>>(
      KeyFromB64(dict.FindString("identity_pub")));
}

std::optional<std::vector<DirectoryAccount>> ParseDirectory(
    const base::DictValue& dict) {
  std::vector<DirectoryAccount> accounts;
  if (const base::ListValue* list = dict.FindList("accounts")) {
    for (const base::Value& value : *list) {
      if (!value.is_dict()) {
        continue;
      }
      const base::DictValue& entry = value.GetDict();
      std::optional<X25519Key> key =
          KeyFromB64(entry.FindString("identity_pub"));
      if (!key) {
        continue;
      }
      accounts.push_back({Str(entry, "client_id"), *key, Str(entry, "name"),
                          Str(entry, "email")});
    }
  }
  return accounts;
}

std::string RoomPath(std::string_view room_id, std::string_view rest = {}) {
  return base::StrCat({"rooms/", base::EscapeAllExceptUnreserved(room_id),
                       rest});
}

}  // namespace

RoomMember::RoomMember() = default;
RoomMember::RoomMember(const RoomMember&) = default;
RoomMember& RoomMember::operator=(const RoomMember&) = default;
RoomMember::~RoomMember() = default;

RoomInfo::RoomInfo() = default;
RoomInfo::RoomInfo(const RoomInfo&) = default;
RoomInfo& RoomInfo::operator=(const RoomInfo&) = default;
RoomInfo::RoomInfo(RoomInfo&&) = default;
RoomInfo& RoomInfo::operator=(RoomInfo&&) = default;
RoomInfo::~RoomInfo() = default;

const RoomMember* RoomInfo::FindMember(std::string_view client_id) const {
  for (const RoomMember& member : members) {
    if (base::EqualsCaseInsensitiveASCII(member.client_id, client_id)) {
      return &member;
    }
  }
  return nullptr;
}

RoomRecord::RoomRecord() = default;
RoomRecord::RoomRecord(const RoomRecord&) = default;
RoomRecord& RoomRecord::operator=(const RoomRecord&) = default;
RoomRecord::RoomRecord(RoomRecord&&) = default;
RoomRecord& RoomRecord::operator=(RoomRecord&&) = default;
RoomRecord::~RoomRecord() = default;

RoomChanges::RoomChanges() = default;
RoomChanges::RoomChanges(RoomChanges&&) = default;
RoomChanges& RoomChanges::operator=(RoomChanges&&) = default;
RoomChanges::~RoomChanges() = default;

RecordWrite::RecordWrite() = default;
RecordWrite::RecordWrite(const RecordWrite&) = default;
RecordWrite& RecordWrite::operator=(const RecordWrite&) = default;
RecordWrite::RecordWrite(RecordWrite&&) = default;
RecordWrite& RecordWrite::operator=(RecordWrite&&) = default;
RecordWrite::~RecordWrite() = default;

CommitResult::CommitResult() = default;
CommitResult::CommitResult(CommitResult&&) = default;
CommitResult& CommitResult::operator=(CommitResult&&) = default;
CommitResult::~CommitResult() = default;

RoomError::RoomError() = default;
RoomError::RoomError(int status, std::string code)
    : status(status), code(std::move(code)) {}
RoomError::RoomError(const RoomError&) = default;
RoomError& RoomError::operator=(const RoomError&) = default;
RoomError::RoomError(RoomError&&) = default;
RoomError& RoomError::operator=(RoomError&&) = default;
RoomError::~RoomError() = default;

RoomClient::NewMember::NewMember() = default;
RoomClient::NewMember::NewMember(const NewMember&) = default;
RoomClient::NewMember& RoomClient::NewMember::operator=(const NewMember&) =
    default;
RoomClient::NewMember::~NewMember() = default;

RoomClient::RoomClient(
    const GURL& server_url,
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    TokenGetter token_getter,
    ClientIdGetter client_id_getter)
    : server_url_(server_url),
      loader_factory_(std::move(loader_factory)),
      token_getter_(std::move(token_getter)),
      client_id_getter_(std::move(client_id_getter)) {}

RoomClient::~RoomClient() = default;

// static
GURL RoomClient::MakeUrl(const GURL& server_url,
                         std::string_view path,
                         std::string_view query,
                         std::string_view client_id) {
  std::string full(server_url.GetPath());
  while (!full.empty() && full.back() == '/') {
    full.pop_back();
  }
  base::StrAppend(&full, {"/compass/", path});
  std::string q(query);
  if (!client_id.empty()) {
    base::StrAppend(&q, {q.empty() ? "" : "&", "client_id=",
                         base::EscapeQueryParamValue(client_id,
                                                     /*use_plus=*/false)});
  }
  GURL::Replacements replacements;
  replacements.SetPathStr(full);
  if (q.empty()) {
    replacements.ClearQuery();
  } else {
    replacements.SetQueryStr(q);
  }
  replacements.ClearRef();
  return server_url.ReplaceComponents(replacements);
}

// static
std::optional<RoomRecord> RoomClient::ParseRecord(const base::DictValue& dict) {
  RoomRecord record;
  record.id = Str(dict, "id");
  record.seq = Int(dict, "seq");
  record.epoch = static_cast<uint32_t>(Int(dict, "epoch"));
  record.deleted = dict.FindBool("deleted").value_or(false);
  record.author = Str(dict, "author");
  if (record.id.empty()) {
    return std::nullopt;
  }
  if (const std::string* ciphertext = dict.FindString("ciphertext")) {
    std::optional<std::vector<uint8_t>> bytes = base::Base64Decode(*ciphertext);
    if (!bytes) {
      return std::nullopt;
    }
    record.ciphertext = std::move(*bytes);
  }
  return record;
}

// static
std::optional<RoomInfo> RoomClient::ParseRoom(const base::DictValue& dict) {
  RoomInfo room;
  room.id = Str(dict, "id");
  if (room.id.empty()) {
    return std::nullopt;
  }
  room.kind = Str(dict, "kind");
  room.owner = Str(dict, "owner");
  room.created = Int(dict, "created");
  room.epoch = static_cast<uint32_t>(Int(dict, "epoch"));
  room.seq = Int(dict, "seq");
  room.bytes = Int(dict, "bytes");
  room.role = Str(dict, "role");
  room.pending = dict.FindBool("pending").value_or(false);
  if (const base::ListValue* members = dict.FindList("members")) {
    for (const base::Value& value : *members) {
      if (value.is_dict()) {
        if (std::optional<RoomMember> member = ParseMember(value.GetDict())) {
          room.members.push_back(std::move(*member));
        }
      }
    }
  }
  if (const base::ListValue* envelopes = dict.FindList("envelopes")) {
    for (const base::Value& value : *envelopes) {
      if (!value.is_dict()) {
        continue;
      }
      std::optional<std::vector<uint8_t>> sealed =
          FromB64(value.GetDict().FindString("sealed"));
      const int64_t epoch = Int(value.GetDict(), "epoch");
      if (sealed && epoch > 0) {
        room.envelopes.push_back(
            {static_cast<uint32_t>(epoch), std::move(*sealed)});
      }
    }
  }
  if (const base::ListValue* invites = dict.FindList("invites")) {
    for (const base::Value& value : *invites) {
      if (value.is_dict()) {
        if (std::optional<RoomInvite> invite = ParseInvite(value.GetDict())) {
          room.invites.push_back(std::move(*invite));
        }
      }
    }
  }
  return room;
}

// static
RoomError RoomClient::ParseError(int status, const std::string& body) {
  RoomError error(status, std::string());
  std::optional<base::Value> value =
      base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return error;
  }
  const base::DictValue& dict = value->GetDict();
  error.code = Str(dict, "error");
  if (const base::ListValue* current = dict.FindList("current")) {
    for (const base::Value& entry : *current) {
      if (entry.is_dict()) {
        if (std::optional<RoomRecord> record = ParseRecord(entry.GetDict())) {
          error.current.push_back(std::move(*record));
        }
      }
    }
  }
  for (auto [key, out] : {std::pair{"missing", &error.missing},
                          std::pair{"unknown", &error.unknown}}) {
    if (const base::ListValue* list = dict.FindList(key)) {
      for (const base::Value& entry : *list) {
        if (entry.is_string()) {
          out->push_back(entry.GetString());
        }
      }
    }
  }
  return error;
}

void RoomClient::Send(std::string_view method,
                      std::string_view path,
                      std::string_view query,
                      const base::DictValue* body,
                      base::TimeDelta timeout,
                      RawCallback callback,
                      std::optional<std::string> linked_token) {
  const std::optional<std::string> token = token_getter_.Run();
  if (!token) {
    std::move(callback).Run(0, std::nullopt);
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = MakeUrl(server_url_, path, query, client_id_getter_.Run());
  request->method = std::string(method);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", *token}));
  if (linked_token) {
    request->headers.SetHeader(kLinkedTokenHeader, *linked_token);
  }
  loaders_.push_front(
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation));
  Loaders::iterator loader = loaders_.begin();
  if (body) {
    // Whole numbers (epochs, seqs, times) as integers: the server's are
    // int64s, and "1.0" isn't one.
    (*loader)->AttachStringForUpload(
        base::WriteJsonWithOptions(
            *body, base::JsonOptions::OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION)
            .value_or("{}"),
        "application/json");
  }
  (*loader)->SetTimeoutDuration(timeout);
  (*loader)->SetAllowHttpErrorResults(true);
  (*loader)->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&RoomClient::OnResponse, weak_factory_.GetWeakPtr(),
                     loader, std::move(callback)),
      kMaxReplySize);
}

void RoomClient::OnResponse(Loaders::iterator loader,
                            RawCallback callback,
                            std::optional<std::string> body) {
  const network::mojom::URLResponseHead* head = (*loader)->ResponseInfo();
  const int status =
      head && head->headers ? head->headers->response_code() : 0;
  loaders_.erase(loader);
  std::move(callback).Run(status, std::move(body));
}

template <typename T>
void RoomClient::SendForJson(
    std::string_view method,
    std::string_view path,
    std::string_view query,
    const base::DictValue* body,
    base::TimeDelta timeout,
    base::OnceCallback<std::optional<T>(const base::DictValue&)> parse,
    Callback<T> callback) {
  Send(method, path, query, body, timeout,
       base::BindOnce(
           [](base::OnceCallback<std::optional<T>(const base::DictValue&)>
                  parse,
              Callback<T> callback, int status,
              std::optional<std::string> body) {
             if (status < 200 || status >= 300) {
               std::move(callback).Run(base::unexpected(
                   status == 0 ? RoomError()
                               : ParseError(status, body.value_or(""))));
               return;
             }
             std::optional<base::Value> value =
                 body ? base::JSONReader::Read(*body, base::JSON_PARSE_RFC)
                      : std::nullopt;
             std::optional<T> parsed =
                 value && value->is_dict()
                     ? std::move(parse).Run(value->GetDict())
                     : std::nullopt;
             if (!parsed) {
               std::move(callback).Run(
                   base::unexpected(RoomError(0, "bad_reply")));
               return;
             }
             std::move(callback).Run(std::move(*parsed));
           },
           std::move(parse), std::move(callback)));
}

void RoomClient::SendForDone(std::string_view method,
                             std::string_view path,
                             const base::DictValue* body,
                             DoneCallback callback,
                             std::optional<std::string> linked_token) {
  Send(method, path, {}, body, kRequestTimeout,
       base::BindOnce(
           [](DoneCallback callback, int status,
              std::optional<std::string> body) {
             if (status >= 200 && status < 300) {
               std::move(callback).Run(std::nullopt);
               return;
             }
             std::move(callback).Run(
                 status == 0 ? RoomError()
                             : ParseError(status, body.value_or("")));
           },
           std::move(callback)),
       std::move(linked_token));
}

void RoomClient::GetInfo(Callback<ServerInfo> callback) {
  SendForJson<ServerInfo>("GET", "info", {}, nullptr, kRequestTimeout,
                          base::BindOnce(&ParseInfo), std::move(callback));
}

void RoomClient::ListRooms(Callback<std::vector<RoomSummary>> callback) {
  SendForJson<std::vector<RoomSummary>>("GET", "rooms", {}, nullptr,
                                        kRequestTimeout,
                                        base::BindOnce(&ParseRoomList),
                                        std::move(callback));
}

void RoomClient::CreateRoom(std::string_view room_id,
                            std::string_view kind,
                            const X25519Key& identity_pub,
                            const std::vector<uint8_t>& envelope,
                            const std::vector<uint8_t>& profile,
                            Callback<RoomInfo> callback) {
  base::DictValue body;
  if (!room_id.empty()) {
    body.Set("id", room_id);
  }
  body.Set("kind", kind);
  body.Set("identity_pub", B64(identity_pub));
  body.Set("envelope", B64(envelope));
  if (!profile.empty()) {
    body.Set("profile", B64(profile));
  }
  SendForJson<RoomInfo>("POST", "rooms", {}, &body, kRequestTimeout,
                        base::BindOnce(&RoomClient::ParseRoom),
                        std::move(callback));
}

void RoomClient::GetRoom(std::string_view room_id,
                         Callback<RoomInfo> callback) {
  SendForJson<RoomInfo>("GET", RoomPath(room_id), {}, nullptr, kRequestTimeout,
                        base::BindOnce(&RoomClient::ParseRoom),
                        std::move(callback));
}

void RoomClient::DeleteRoom(std::string_view room_id, DoneCallback callback) {
  SendForDone("DELETE", RoomPath(room_id), nullptr, std::move(callback));
}

void RoomClient::GetChanges(std::string_view room_id,
                            int64_t since,
                            int wait_seconds,
                            Callback<RoomChanges> callback) {
  wait_seconds = std::clamp(wait_seconds, 0, kMaxWaitSeconds);
  const std::string query =
      base::StrCat({"since=", base::NumberToString(since),
                    wait_seconds ? "&wait=" : "",
                    wait_seconds ? base::NumberToString(wait_seconds) : ""});
  SendForJson<RoomChanges>(
      "GET", RoomPath(room_id, "/changes"), query, nullptr,
      kRequestTimeout + base::Seconds(wait_seconds) + kLongPollSlack,
      base::BindOnce(&ParseChanges), std::move(callback));
}

void RoomClient::CommitRecords(std::string_view room_id,
                               const std::vector<RecordWrite>& writes,
                               Callback<CommitResult> callback) {
  base::ListValue records;
  for (const RecordWrite& write : writes) {
    base::DictValue record;
    record.Set("id", write.id);
    record.Set("epoch", static_cast<double>(write.epoch));
    if (write.deleted) {
      record.Set("deleted", true);
    } else {
      record.Set("ciphertext", B64(write.ciphertext));
    }
    if (write.base_seq) {
      record.Set("base_seq", static_cast<double>(*write.base_seq));
    }
    records.Append(std::move(record));
  }
  base::DictValue body;
  body.Set("records", std::move(records));
  SendForJson<CommitResult>("POST", RoomPath(room_id, "/records"), {}, &body,
                            base::Minutes(2), base::BindOnce(&ParseCommit),
                            std::move(callback));
}

void RoomClient::CreateInvite(std::string_view room_id,
                              std::string_view role,
                              std::string_view secret_hash,
                              int64_t expires,
                              int uses,
                              Callback<RoomInvite> callback) {
  base::DictValue body;
  body.Set("role", role);
  body.Set("secret_hash", secret_hash);
  if (expires) {
    body.Set("expires", static_cast<double>(expires));
  }
  if (uses) {
    body.Set("uses", uses);
  }
  SendForJson<RoomInvite>("POST", RoomPath(room_id, "/invites"), {}, &body,
                          kRequestTimeout, base::BindOnce(&ParseInviteAnswer),
                          std::move(callback));
}

void RoomClient::RevokeInvite(std::string_view room_id,
                              std::string_view invite_id,
                              DoneCallback callback) {
  SendForDone(
      "DELETE",
      RoomPath(room_id, base::StrCat({"/invites/", base::EscapeAllExceptUnreserved(
                                                       invite_id)})),
      nullptr, std::move(callback));
}

void RoomClient::Join(std::string_view room_id,
                      std::string_view invite_id,
                      std::string_view secret,
                      const X25519Key& identity_pub,
                      const std::vector<uint8_t>& profile,
                      Callback<RoomInfo> callback) {
  base::DictValue body;
  if (!invite_id.empty()) {
    body.Set("invite_id", invite_id);
    body.Set("secret", secret);
  }
  body.Set("identity_pub", B64(identity_pub));
  if (!profile.empty()) {
    body.Set("profile", B64(profile));
  }
  SendForJson<RoomInfo>("POST", RoomPath(room_id, "/join"), {}, &body,
                        kRequestTimeout,
                        base::BindOnce(&RoomClient::ParseRoom),
                        std::move(callback));
}

void RoomClient::AddMembers(std::string_view room_id,
                            const std::vector<NewMember>& members,
                            Callback<RoomInfo> callback) {
  base::ListValue list;
  for (const NewMember& member : members) {
    base::ListValue envelopes;
    for (const RoomEnvelope& envelope : member.envelopes) {
      envelopes.Append(
          base::DictValue()
              .Set("epoch", static_cast<double>(envelope.epoch))
              .Set("sealed", B64(envelope.sealed)));
    }
    list.Append(base::DictValue()
                    .Set("client_id", member.client_id)
                    .Set("role", member.role)
                    .Set("identity_pub", B64(member.identity_pub))
                    .Set("envelopes", std::move(envelopes)));
  }
  base::DictValue body;
  body.Set("members", std::move(list));
  SendForJson<RoomInfo>("POST", RoomPath(room_id, "/members"), {}, &body,
                        kRequestTimeout,
                        base::BindOnce(&RoomClient::ParseRoom),
                        std::move(callback));
}

void RoomClient::UpdateMember(std::string_view room_id,
                              std::string_view client_id,
                              std::optional<std::string> role,
                              std::optional<std::vector<uint8_t>> profile,
                              DoneCallback callback) {
  base::DictValue body;
  if (role) {
    body.Set("role", *role);
  }
  if (profile) {
    if (profile->empty()) {
      body.Set("profile", base::Value());
    } else {
      body.Set("profile", B64(*profile));
    }
  }
  SendForDone("PATCH",
              RoomPath(room_id, base::StrCat({"/members/", client_id})),
              &body, std::move(callback));
}

void RoomClient::RemoveMember(std::string_view room_id,
                              std::string_view client_id,
                              DoneCallback callback) {
  SendForDone("DELETE",
              RoomPath(room_id, base::StrCat({"/members/", client_id})),
              nullptr, std::move(callback));
}

void RoomClient::TransferOwner(std::string_view room_id,
                               std::string_view client_id,
                               DoneCallback callback) {
  base::DictValue body;
  body.Set("client_id", client_id);
  SendForDone("POST", RoomPath(room_id, "/owner"), &body, std::move(callback));
}

void RoomClient::MoveToSuccessor(std::string_view room_id,
                                 std::string_view linked_token,
                                 const X25519Key& identity_pub,
                                 const std::vector<RoomEnvelope>& envelopes,
                                 DoneCallback callback) {
  base::ListValue list;
  for (const RoomEnvelope& envelope : envelopes) {
    list.Append(base::DictValue()
                    .Set("epoch", static_cast<double>(envelope.epoch))
                    .Set("sealed", B64(envelope.sealed)));
  }
  base::DictValue body;
  body.Set("identity_pub", B64(identity_pub));
  body.Set("envelopes", std::move(list));
  SendForDone("POST", RoomPath(room_id, "/successor"), &body,
              std::move(callback), std::string(linked_token));
}

void RoomClient::Rotate(
    std::string_view room_id,
    uint32_t epoch,
    const std::map<std::string, std::vector<uint8_t>>& envelopes,
    DoneCallback callback) {
  base::ListValue list;
  for (const auto& [client_id, sealed] : envelopes) {
    list.Append(base::DictValue()
                    .Set("client_id", client_id)
                    .Set("sealed", B64(sealed)));
  }
  base::DictValue body;
  body.Set("epoch", static_cast<double>(epoch));
  body.Set("envelopes", std::move(list));
  SendForDone("POST", RoomPath(room_id, "/epochs"), &body,
              std::move(callback));
}

void RoomClient::AddEnvelopes(std::string_view room_id,
                              const std::vector<MemberEnvelope>& envelopes,
                              DoneCallback callback) {
  base::ListValue list;
  for (const MemberEnvelope& envelope : envelopes) {
    list.Append(base::DictValue()
                    .Set("client_id", envelope.client_id)
                    .Set("epoch", static_cast<double>(envelope.epoch))
                    .Set("sealed", B64(envelope.sealed)));
  }
  base::DictValue body;
  body.Set("envelopes", std::move(list));
  SendForDone("POST", RoomPath(room_id, "/envelopes"), &body,
              std::move(callback));
}

void RoomClient::GetIdentity(Callback<std::optional<X25519Key>> callback) {
  SendForJson<std::optional<X25519Key>>(
      "GET", "identity", {}, nullptr, kRequestTimeout,
      base::BindOnce(&ParseIdentity), std::move(callback));
}

void RoomClient::PutIdentity(const X25519Key& identity_pub,
                             DoneCallback callback) {
  base::DictValue body;
  body.Set("identity_pub", B64(identity_pub));
  SendForDone("PUT", "identity", &body, std::move(callback));
}

void RoomClient::LookUpDirectory(
    std::string_view email,
    Callback<std::vector<DirectoryAccount>> callback) {
  SendForJson<std::vector<DirectoryAccount>>(
      "GET", "directory",
      base::StrCat({"email=", base::EscapeQueryParamValue(email, false)}),
      nullptr, kRequestTimeout, base::BindOnce(&ParseDirectory),
      std::move(callback));
}

}  // namespace compass::sync::rooms
