// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/rooms_bridge.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/android/spaces_android.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/components/sync/rooms/room_client.h"

// Must come after the class declaration.
#include "compass/android/jni_headers/RoomsBridge_jni.h"
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace compass {

namespace {

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("{}");
}

base::DictValue StateDict(const rooms::SharedState& state) {
  return base::DictValue()
      .Set("shared", state.shared)
      .Set("view_only", state.read_only)
      .Set("tooltip", state.tooltip);
}

}  // namespace

RoomsBridge::RoomsBridge(JNIEnv* env,
                         const base::android::JavaRef<jobject>& java_object,
                         Profile* profile)
    : java_object_(env, java_object),
      profile_(profile),
      rooms_(rooms::RoomService::Get(profile)) {
  if (rooms_) {
    observation_.Observe(rooms_.get());
  }
}

RoomsBridge::~RoomsBridge() = default;

void RoomsBridge::Destroy(JNIEnv* env) {
  delete this;
}

std::string RoomsBridge::Snapshot(JNIEnv* env) {
  base::DictValue snapshot;
  snapshot.Set("available", rooms_ && rooms_->IsAvailable());
  base::ListValue notices;
  if (rooms_) {
    for (const rooms::RemovedNotice& notice : rooms_->GetRemovedNotices()) {
      notices.Append(base::DictValue()
                         .Set("room_id", notice.room_id)
                         .Set("text", rooms::RemovedNoticeText(notice)));
    }
  }
  snapshot.Set("notices", std::move(notices));
  base::DictValue shared;
  if (rooms_) {
    for (const auto& [uuid, state] : rooms_->GetSharedRoots()) {
      shared.Set(uuid, state.tooltip);
    }
  }
  snapshot.Set("shared", std::move(shared));
  return Json(base::Value(std::move(snapshot)));
}

std::string RoomsBridge::Item(JNIEnv* env,
                              const std::string& kind,
                              const std::string& id) {
  if (!rooms_) {
    return std::string();
  }
  std::string name;
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  SpacesAndroid* spaces = SpacesAndroid::GetIfExists(profile_);
  if (kind == "space" && spaces) {
    if (SpaceModel* space = spaces->list().FindSpace(uuid)) {
      name = base::UTF16ToUTF8(space->name());
    }
  } else if (kind == "folder" && spaces) {
    if (SidebarNode* node = spaces->FindNode(uuid)) {
      name = base::UTF16ToUTF8(node->title());
    }
  } else if (kind == "easel") {
    if (easels::EaselStore* store = easels::EaselStore::Get(profile_)) {
      if (const sync_pb::CompassEasel* easel = store->FindEasel(id)) {
        name = easel->title();
      }
    }
  }
  const std::string room_id = rooms_->FindRoomOf(id);
  if (name.empty() && room_id.empty()) {
    return std::string();
  }
  return Json(base::Value(base::DictValue()
                              .Set("kind", kind)
                              .Set("id", id)
                              .Set("name", name)
                              .Set("room_id", room_id)
                              .Set("can_share", rooms_->IsAvailable())));
}

std::string RoomsBridge::NodeState(JNIEnv* env, const std::string& id) {
  SpacesAndroid* spaces = SpacesAndroid::GetIfExists(profile_);
  SidebarNode* node =
      spaces ? spaces->FindNode(base::Uuid::ParseLowercase(id)) : nullptr;
  if (!rooms_ || !node) {
    return Json(base::Value(StateDict(rooms::SharedState())));
  }
  rooms::SharedState state = rooms_->GetNodeState(*node);
  // The badge goes on the shared item itself.
  if (state.shared && node->parent() &&
      node->parent()->type() != SidebarNode::Type::kRoot &&
      rooms_->GetNodeState(*node->parent()).shared) {
    state.tooltip.clear();
  }
  return Json(base::Value(StateDict(state)));
}

std::string RoomsBridge::SpaceState(JNIEnv* env, const std::string& id) {
  return Json(base::Value(StateDict(
      rooms_ ? rooms_->GetSpaceState(id) : rooms::SharedState())));
}

void RoomsBridge::Answer(int32_t request, base::DictValue result) {
  Java_RoomsBridge_onResult(jni_zero::AttachCurrentThread(), java_object_,
                            request, Json(base::Value(std::move(result))));
}

rooms::RoomService::Done RoomsBridge::DoneFor(int32_t request) {
  return base::BindOnce(
      [](base::WeakPtr<RoomsBridge> self, int32_t request, const std::string& error) {
        if (self) {
          self->Answer(request, base::DictValue().Set("error", error));
        }
      },
      weak_factory_.GetWeakPtr(), request);
}

void RoomsBridge::Share(JNIEnv* env,
                        int32_t request,
                        const std::string& kind,
                        const std::string& id) {
  if (!rooms_) {
    Answer(request, base::DictValue().Set(
                        "error", l10n_util::GetStringUTF8(
                                     IDS_COMPASS_ROOMS_SHARING_UNAVAILABLE)));
    return;
  }
  rooms_->Share(kind, id,
                base::BindOnce(
                    [](base::WeakPtr<RoomsBridge> self, int32_t request,
                       const std::string& room_id, const std::string& error) {
                      if (self) {
                        self->Answer(request, base::DictValue()
                                                  .Set("room_id", room_id)
                                                  .Set("error", error));
                      }
                    },
                    weak_factory_.GetWeakPtr(), request));
}

void RoomsBridge::GetRoom(JNIEnv* env,
                          int32_t request,
                          const std::string& room_id) {
  if (!rooms_) {
    Answer(request,
           base::DictValue().Set("error", l10n_util::GetStringUTF8(
                                              IDS_COMPASS_ROOMS_NOT_SHARED)));
    return;
  }
  rooms_->GetRoomView(
      room_id,
      base::BindOnce(
          [](base::WeakPtr<RoomsBridge> self, int32_t request,
             std::optional<rooms::RoomView> view, const std::string& error) {
            if (!self) {
              return;
            }
            base::DictValue result;
            result.Set("error", error);
            if (view) {
              base::ListValue members;
              for (const rooms::MemberView& m : view->members) {
                members.Append(base::DictValue()
                                   .Set("client_id", m.client_id)
                                   .Set("name", m.name)
                                   .Set("email", m.email)
                                   .Set("role", m.role)
                                   .Set("pending", m.pending)
                                   .Set("is_me", m.is_me));
              }
              base::ListValue invites;
              for (const rooms::InviteView& i : view->invites) {
                invites.Append(base::DictValue()
                                   .Set("id", i.id)
                                   .Set("role", i.role)
                                   .Set("expires", static_cast<double>(i.expires))
                                   .Set("uses_left", i.uses_left)
                                   .Set("revoked", i.revoked));
              }
              result.Set("room",
                         base::DictValue()
                             .Set("room_id", view->room_id)
                             .Set("kind", view->kind)
                             .Set("name", view->name)
                             .Set("role", view->role)
                             .Set("server", view->server)
                             .Set("oidc", view->oidc)
                             .Set("members", std::move(members))
                             .Set("invites", std::move(invites)));
            }
            self->Answer(request, std::move(result));
          },
          weak_factory_.GetWeakPtr(), request));
}

void RoomsBridge::CreateInviteLink(JNIEnv* env,
                                   int32_t request,
                                   const std::string& room_id,
                                   const std::string& role,
                                   int32_t expires_days,
                                   int32_t uses) {
  if (!rooms_ || (role != sync::rooms::kRoleEditor &&
                  role != sync::rooms::kRoleViewer)) {
    Answer(request,
           base::DictValue().Set("error", l10n_util::GetStringUTF8(
                                              IDS_COMPASS_ROOMS_NOT_SHARED)));
    return;
  }
  rooms_->CreateInviteLink(
      room_id, role, base::Days(std::clamp(expires_days, 0, 365)),
      std::clamp(uses, 0, 1000),
      base::BindOnce(
          [](base::WeakPtr<RoomsBridge> self, int32_t request,
             const std::string& link, const std::string& error) {
            if (self) {
              self->Answer(request,
                           base::DictValue().Set("link", link).Set("error",
                                                                   error));
            }
          },
          weak_factory_.GetWeakPtr(), request));
}

void RoomsBridge::RevokeInvite(JNIEnv* env,
                               int32_t request,
                               const std::string& room_id,
                               const std::string& invite_id) {
  if (rooms_) {
    rooms_->RevokeInvite(room_id, invite_id, DoneFor(request));
  }
}

void RoomsBridge::InviteByEmail(JNIEnv* env,
                                int32_t request,
                                const std::string& room_id,
                                const std::string& email,
                                const std::string& role) {
  if (rooms_) {
    rooms_->InviteByEmail(room_id, email, role, DoneFor(request));
  }
}

void RoomsBridge::ChangeRole(JNIEnv* env,
                             int32_t request,
                             const std::string& room_id,
                             const std::string& client_id,
                             const std::string& role) {
  if (rooms_) {
    rooms_->ChangeRole(room_id, client_id, role, DoneFor(request));
  }
}

void RoomsBridge::RemoveMember(JNIEnv* env,
                               int32_t request,
                               const std::string& room_id,
                               const std::string& client_id) {
  if (rooms_) {
    rooms_->RemoveMember(room_id, client_id, DoneFor(request));
  }
}

void RoomsBridge::Leave(JNIEnv* env,
                        int32_t request,
                        const std::string& room_id) {
  if (rooms_) {
    rooms_->Leave(room_id, DoneFor(request));
  }
}

void RoomsBridge::DeleteRoom(JNIEnv* env,
                             int32_t request,
                             const std::string& room_id) {
  if (rooms_) {
    rooms_->DeleteRoom(room_id, DoneFor(request));
  }
}

std::string RoomsBridge::PreviewJoin(JNIEnv* env, const std::string& link) {
  std::optional<rooms::JoinPreview> preview =
      rooms_ ? rooms_->PreviewJoin(link) : std::nullopt;
  if (!preview) {
    return std::string();
  }
  return Json(base::Value(base::DictValue()
                              .Set("name", preview->name)
                              .Set("kind", preview->kind)
                              .Set("server", preview->server)
                              .Set("already_joined", preview->already_joined)));
}

std::string RoomsBridge::Spaces(JNIEnv* env) {
  base::ListValue list;
  if (SpacesAndroid* spaces = SpacesAndroid::GetIfExists(profile_)) {
    SpaceList& space_list = spaces->list();
    for (const auto& space : space_list.spaces()) {
      list.Append(base::DictValue()
                      .Set("id", space->id().AsLowercaseString())
                      .Set("name", base::UTF16ToUTF8(space->name()))
                      .Set("active", space.get() == &space_list.active_space()));
    }
  }
  return Json(base::Value(std::move(list)));
}

void RoomsBridge::Join(JNIEnv* env,
                       int32_t request,
                       const std::string& link,
                       const std::string& space_id) {
  if (!rooms_) {
    Answer(request, base::DictValue().Set(
                        "error", l10n_util::GetStringUTF8(
                                     IDS_COMPASS_ROOMS_SHARING_UNAVAILABLE)));
    return;
  }
  rooms_->Join(link, space_id,
               base::BindOnce(
                   [](base::WeakPtr<RoomsBridge> self, int32_t request,
                      const std::string& root, const std::string& error) {
                     if (self) {
                       self->Answer(request, base::DictValue()
                                                 .Set("root", root)
                                                 .Set("error", error));
                     }
                   },
                   weak_factory_.GetWeakPtr(), request));
}

std::string RoomsBridge::GetDisplayName(JNIEnv* env) {
  return rooms_ ? rooms_->GetDisplayName() : std::string();
}

void RoomsBridge::SetDisplayName(JNIEnv* env, const std::string& name) {
  if (rooms_) {
    rooms_->SetDisplayName(name);
  }
}

void RoomsBridge::DismissNotice(JNIEnv* env, const std::string& room_id) {
  if (rooms_) {
    rooms_->DismissRemovedNotice(room_id);
  }
}

void RoomsBridge::OnRoomsChanged() {
  Java_RoomsBridge_onChanged(jni_zero::AttachCurrentThread(), java_object_);
}

static int64_t JNI_RoomsBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<int64_t>(new RoomsBridge(env, caller, profile));
}

}  // namespace compass

DEFINE_JNI(RoomsBridge)
