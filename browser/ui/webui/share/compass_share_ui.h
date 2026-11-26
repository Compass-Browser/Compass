// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SHARE_COMPASS_SHARE_UI_H_
#define COMPASS_BROWSER_UI_WEBUI_SHARE_COMPASS_SHARE_UI_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/rooms/room_service.h"
#include "compass/browser/ui/webui/share/share.mojom.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "url/gurl.h"

namespace content {
class WebContents;
class WebUI;
}  // namespace content

namespace compass {

// chrome://compass-share (compass://share): the share sheet of a space,
// folder or easel (?kind=folder&id=<uuid>), in a bubble (ShowShareBubble()),
// and the join page of an invite link (#join=<the link's fragment>), in a
// tab. docs/design/sync.md, "Rooms: clients".
inline constexpr char kCompassShareHost[] = "compass-share";

// The share sheet's URL for an item.
GURL ShareSheetUrl(const std::string& kind, const std::string& id);
// The join page's URL for an invite link's fragment (base64url).
GURL JoinPageUrl(const std::string& fragment);
class CompassShareUI;

class CompassShareUIConfig
    : public DefaultTopChromeWebUIConfig<CompassShareUI> {
 public:
  CompassShareUIConfig();

  // DefaultTopChromeWebUIConfig:
  bool ShouldAutoResizeHost() override;
};

class SharePageHandler : public share::mojom::PageHandler,
                         public rooms::RoomService::Observer {
 public:
  SharePageHandler(mojo::PendingReceiver<share::mojom::PageHandler> receiver,
                   mojo::PendingRemote<share::mojom::Page> page,
                   CompassShareUI* controller,
                   rooms::RoomService* rooms);
  SharePageHandler(const SharePageHandler&) = delete;
  SharePageHandler& operator=(const SharePageHandler&) = delete;
  ~SharePageHandler() override;

  // share::mojom::PageHandler:
  void GetItem(const std::string& kind,
               const std::string& id,
               GetItemCallback callback) override;
  void Share(const std::string& kind,
             const std::string& id,
             ShareCallback callback) override;
  void GetRoom(const std::string& room_id, GetRoomCallback callback) override;
  void CreateInviteLink(const std::string& room_id,
                        const std::string& role,
                        int32_t expires_days,
                        int32_t uses,
                        CreateInviteLinkCallback callback) override;
  void RevokeInvite(const std::string& room_id,
                    const std::string& invite_id,
                    RevokeInviteCallback callback) override;
  void InviteByEmail(const std::string& room_id,
                     const std::string& email,
                     const std::string& role,
                     InviteByEmailCallback callback) override;
  void ChangeRole(const std::string& room_id,
                  const std::string& client_id,
                  const std::string& role,
                  ChangeRoleCallback callback) override;
  void RemoveMember(const std::string& room_id,
                    const std::string& client_id,
                    RemoveMemberCallback callback) override;
  void Leave(const std::string& room_id, LeaveCallback callback) override;
  void DeleteRoom(const std::string& room_id,
                  DeleteRoomCallback callback) override;
  void CopyToClipboard(const std::string& text) override;
  void PreviewJoin(const std::string& link,
                   PreviewJoinCallback callback) override;
  void GetSpaces(GetSpacesCallback callback) override;
  void Join(const std::string& link,
            const std::string& space_id,
            JoinCallback callback) override;
  void GetDisplayName(GetDisplayNameCallback callback) override;
  void SetDisplayName(const std::string& name) override;
  void GetLook(GetLookCallback callback) override;
  void ShowUI() override;
  void Close() override;

  // rooms::RoomService::Observer:
  void OnRoomsChanged() override;

 private:
  mojo::Receiver<share::mojom::PageHandler> receiver_;
  mojo::Remote<share::mojom::Page> page_;
  const raw_ptr<CompassShareUI> controller_;
  const raw_ptr<content::WebUI> web_ui_;
  const raw_ptr<rooms::RoomService> rooms_;
  base::ScopedObservation<rooms::RoomService, rooms::RoomService::Observer>
      observation_{this};
};

class CompassShareUI : public TopChromeWebUIController,
                       public share::mojom::PageHandlerFactory {
 public:
  explicit CompassShareUI(content::WebUI* web_ui);
  CompassShareUI(const CompassShareUI&) = delete;
  CompassShareUI& operator=(const CompassShareUI&) = delete;
  ~CompassShareUI() override;

  void BindInterface(
      mojo::PendingReceiver<share::mojom::PageHandlerFactory> receiver);

  static constexpr std::string_view GetWebUIName() { return "CompassShare"; }

 private:
  // share::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<share::mojom::Page> page,
      mojo::PendingReceiver<share::mojom::PageHandler> receiver) override;

  std::unique_ptr<SharePageHandler> handler_;
  mojo::Receiver<share::mojom::PageHandlerFactory> factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SHARE_COMPASS_SHARE_UI_H_
