// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_ROOMS_BRIDGE_H_
#define COMPASS_BROWSER_ANDROID_ROOMS_BRIDGE_H_

#include <jni.h>

#include <string>

#include "base/android/scoped_java_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/rooms/room_service.h"

class Profile;

namespace compass {

// The Android shell's sharing (RoomsBridge.java, the share sheet and the join
// screen): the profile's RoomService, as the desktop's chrome://compass-share
// uses it (docs/design/sync.md, "Rooms: clients"). Answers come as JSON,
// asynchronous ones through onResult(request, json).
class RoomsBridge : public rooms::RoomService::Observer {
 public:
  RoomsBridge(JNIEnv* env,
              const base::android::JavaRef<jobject>& java_object,
              Profile* profile);
  RoomsBridge(const RoomsBridge&) = delete;
  RoomsBridge& operator=(const RoomsBridge&) = delete;
  ~RoomsBridge() override;

  void Destroy(JNIEnv* env);

  // {"available", "notices": [{"room_id", "text"}]}.
  std::string Snapshot(JNIEnv* env);
  // {"kind", "id", "name", "room_id", "can_share"}, or "" if it isn't here.
  std::string Item(JNIEnv* env, const std::string& kind, const std::string& id);
  // {"shared", "view_only", "tooltip"} of a node, or of a space.
  std::string NodeState(JNIEnv* env, const std::string& id);
  std::string SpaceState(JNIEnv* env, const std::string& id);

  // Asynchronous: onResult(request, {"room_id"|"link"|"room"..., "error"}).
  void Share(JNIEnv* env,
             int32_t request,
             const std::string& kind,
             const std::string& id);
  void GetRoom(JNIEnv* env, int32_t request, const std::string& room_id);
  void CreateInviteLink(JNIEnv* env,
                        int32_t request,
                        const std::string& room_id,
                        const std::string& role,
                        int32_t expires_days,
                        int32_t uses);
  void RevokeInvite(JNIEnv* env,
                    int32_t request,
                    const std::string& room_id,
                    const std::string& invite_id);
  void InviteByEmail(JNIEnv* env,
                     int32_t request,
                     const std::string& room_id,
                     const std::string& email,
                     const std::string& role);
  void ChangeRole(JNIEnv* env,
                  int32_t request,
                  const std::string& room_id,
                  const std::string& client_id,
                  const std::string& role);
  void RemoveMember(JNIEnv* env,
                    int32_t request,
                    const std::string& room_id,
                    const std::string& client_id);
  void Leave(JNIEnv* env, int32_t request, const std::string& room_id);
  void DeleteRoom(JNIEnv* env, int32_t request, const std::string& room_id);
  // {"name", "kind", "server", "already_joined"}, or "" for a bad link.
  std::string PreviewJoin(JNIEnv* env, const std::string& link);
  // [{"id", "name", "active"}]: where a joined folder or easel can go.
  std::string Spaces(JNIEnv* env);
  void Join(JNIEnv* env,
            int32_t request,
            const std::string& link,
            const std::string& space_id);
  std::string GetDisplayName(JNIEnv* env);
  void SetDisplayName(JNIEnv* env, const std::string& name);
  void DismissNotice(JNIEnv* env, const std::string& room_id);

  // rooms::RoomService::Observer:
  void OnRoomsChanged() override;

 private:
  void Answer(int32_t request, base::DictValue result);
  rooms::RoomService::Done DoneFor(int32_t request);

  base::android::ScopedJavaGlobalRef<jobject> java_object_;
  const raw_ptr<Profile> profile_;
  raw_ptr<rooms::RoomService> rooms_;
  base::ScopedObservation<rooms::RoomService, rooms::RoomService::Observer>
      observation_{this};
  base::WeakPtrFactory<RoomsBridge> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_ROOMS_BRIDGE_H_
