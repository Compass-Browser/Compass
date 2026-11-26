// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/share/compass_share_ui.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/ui/webui/bubble/bubble_look_mojo.h"
#include "compass/browser/ui/webui/localized_strings/localized_strings.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/components/sync/rooms/room_invite_link.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "compass/strings/grit/compass_strings.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/webui/webui_util.h"

namespace compass {

namespace {

std::string NameOf(Profile* profile,
                   const std::string& kind,
                   const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  SpaceService* spaces = SpaceService::Get(profile);
  if (kind == "space" && spaces) {
    if (SpaceModel* space = spaces->space_list().FindSpace(uuid)) {
      return base::UTF16ToUTF8(space->name());
    }
  } else if (kind == "folder" && spaces) {
    for (const auto& space : spaces->space_list().spaces()) {
      if (SidebarNode* node = space->FindById(uuid);
          node && node->type() == SidebarNode::Type::kFolder) {
        return base::UTF16ToUTF8(node->title());
      }
    }
  } else if (kind == "easel") {
    if (easels::EaselStore* store = easels::EaselStore::Get(profile)) {
      if (const sync_pb::CompassEasel* easel = store->FindEasel(id)) {
        return easel->title();
      }
    }
  }
  return std::string();
}

}  // namespace

GURL ShareSheetUrl(const std::string& kind, const std::string& id) {
  return GURL(base::StrCat({content::kChromeUIScheme, "://", kCompassShareHost,
                            "/?kind=", kind, "&id=", id}));
}

GURL JoinPageUrl(const std::string& fragment) {
  return GURL(base::StrCat({content::kChromeUIScheme, "://", kCompassShareHost,
                            "/#join=", fragment}));
}

CompassShareUIConfig::CompassShareUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  kCompassShareHost) {}

bool CompassShareUIConfig::ShouldAutoResizeHost() {
  // The bubble grows and shrinks with the sheet.
  return true;
}

CompassShareUI::CompassShareUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kCompassShareHost);
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_SHARE_HTML);
  AddSidebarPakLocalizedStrings(source);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassShareUI::~CompassShareUI() = default;

void CompassShareUI::BindInterface(
    mojo::PendingReceiver<share::mojom::PageHandlerFactory> receiver) {
  factory_receiver_.reset();
  factory_receiver_.Bind(std::move(receiver));
}

void CompassShareUI::CreatePageHandler(
    mojo::PendingRemote<share::mojom::Page> page,
    mojo::PendingReceiver<share::mojom::PageHandler> receiver) {
  rooms::RoomService* rooms =
      rooms::RoomService::Get(Profile::FromWebUI(web_ui()));
  if (!rooms) {
    // Incognito and Guest don't share.
    return;
  }
  handler_ = std::make_unique<SharePageHandler>(
      std::move(receiver), std::move(page), this, rooms);
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassShareUI)

SharePageHandler::SharePageHandler(
    mojo::PendingReceiver<share::mojom::PageHandler> receiver,
    mojo::PendingRemote<share::mojom::Page> page,
    CompassShareUI* controller,
    rooms::RoomService* rooms)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      controller_(controller),
      web_ui_(controller->web_ui()),
      rooms_(rooms) {
  observation_.Observe(rooms);
}

SharePageHandler::~SharePageHandler() = default;

void SharePageHandler::GetItem(const std::string& kind,
                               const std::string& id,
                               GetItemCallback callback) {
  Profile* profile = Profile::FromWebUI(web_ui_);
  std::string name = NameOf(profile, kind, id);
  if (name.empty() && rooms_->FindRoomOf(id).empty()) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto item = share::mojom::Item::New();
  item->kind = kind;
  item->id = id;
  item->name = name;
  item->room_id = rooms_->FindRoomOf(id);
  item->can_share = rooms_->IsAvailable();
  std::move(callback).Run(std::move(item));
}

void SharePageHandler::Share(const std::string& kind,
                             const std::string& id,
                             ShareCallback callback) {
  rooms_->Share(kind, id, std::move(callback));
}

void SharePageHandler::GetRoom(const std::string& room_id,
                               GetRoomCallback callback) {
  rooms_->GetRoomView(
      room_id,
      base::BindOnce(
          [](GetRoomCallback callback, std::optional<rooms::RoomView> view,
             const std::string& error) {
            if (!view) {
              std::move(callback).Run(nullptr, error);
              return;
            }
            auto room = share::mojom::Room::New();
            room->room_id = view->room_id;
            room->kind = view->kind;
            room->name = view->name;
            room->role = view->role;
            room->server = view->server;
            room->oidc = view->oidc;
            for (const rooms::MemberView& m : view->members) {
              room->members.push_back(share::mojom::Member::New(
                  m.client_id, m.name, m.email, m.role, m.pending, m.is_me));
            }
            for (const rooms::InviteView& i : view->invites) {
              room->invites.push_back(share::mojom::Invite::New(
                  i.id, i.role, i.expires, i.uses_left, i.revoked));
            }
            std::move(callback).Run(std::move(room), std::string());
          },
          std::move(callback)));
}

void SharePageHandler::CreateInviteLink(const std::string& room_id,
                                        const std::string& role,
                                        int32_t expires_days,
                                        int32_t uses,
                                        CreateInviteLinkCallback callback) {
  if (role != sync::rooms::kRoleEditor && role != sync::rooms::kRoleViewer) {
    std::move(callback).Run(
        std::string(), l10n_util::GetStringUTF8(IDS_COMPASS_SHARE_NO_ROLE));
    return;
  }
  rooms_->CreateInviteLink(room_id, role,
                           base::Days(std::clamp(expires_days, 0, 365)),
                           std::clamp(uses, 0, 1000), std::move(callback));
}

void SharePageHandler::RevokeInvite(const std::string& room_id,
                                    const std::string& invite_id,
                                    RevokeInviteCallback callback) {
  rooms_->RevokeInvite(room_id, invite_id, std::move(callback));
}

void SharePageHandler::InviteByEmail(const std::string& room_id,
                                     const std::string& email,
                                     const std::string& role,
                                     InviteByEmailCallback callback) {
  rooms_->InviteByEmail(room_id, email, role, std::move(callback));
}

void SharePageHandler::ChangeRole(const std::string& room_id,
                                  const std::string& client_id,
                                  const std::string& role,
                                  ChangeRoleCallback callback) {
  rooms_->ChangeRole(room_id, client_id, role, std::move(callback));
}

void SharePageHandler::RemoveMember(const std::string& room_id,
                                    const std::string& client_id,
                                    RemoveMemberCallback callback) {
  rooms_->RemoveMember(room_id, client_id, std::move(callback));
}

void SharePageHandler::Leave(const std::string& room_id,
                             LeaveCallback callback) {
  rooms_->Leave(room_id, std::move(callback));
}

void SharePageHandler::DeleteRoom(const std::string& room_id,
                                  DeleteRoomCallback callback) {
  rooms_->DeleteRoom(room_id, std::move(callback));
}

void SharePageHandler::CopyToClipboard(const std::string& text) {
  ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
      .WriteText(base::UTF8ToUTF16(text));
}

void SharePageHandler::PreviewJoin(const std::string& link,
                                   PreviewJoinCallback callback) {
  std::optional<rooms::JoinPreview> preview = rooms_->PreviewJoin(link);
  if (!preview) {
    std::move(callback).Run(nullptr);
    return;
  }
  std::move(callback).Run(share::mojom::JoinPreview::New(
      preview->name, preview->kind, preview->server, preview->already_joined));
}

void SharePageHandler::GetSpaces(GetSpacesCallback callback) {
  std::vector<share::mojom::SpaceOptionPtr> options;
  if (SpaceService* spaces =
          SpaceService::Get(Profile::FromWebUI(web_ui_))) {
    SpaceList& list = spaces->space_list();
    for (const auto& space : list.spaces()) {
      options.push_back(share::mojom::SpaceOption::New(
          space->id().AsLowercaseString(), base::UTF16ToUTF8(space->name()),
          space.get() == &list.active_space()));
    }
  }
  std::move(callback).Run(std::move(options));
}

void SharePageHandler::Join(const std::string& link,
                            const std::string& space_id,
                            JoinCallback callback) {
  rooms_->Join(link, space_id,
               base::BindOnce(
                   [](JoinCallback callback, const std::string& root,
                      const std::string& error) {
                     std::move(callback).Run(error);
                   },
                   std::move(callback)));
}

void SharePageHandler::GetDisplayName(GetDisplayNameCallback callback) {
  std::move(callback).Run(rooms_->GetDisplayName());
}

void SharePageHandler::SetDisplayName(const std::string& name) {
  rooms_->SetDisplayName(name);
}

void SharePageHandler::GetLook(GetLookCallback callback) {
  std::move(callback).Run(GetBubbleLookFor(web_ui_->GetWebContents()));
}

void SharePageHandler::ShowUI() {
  if (controller_->embedder()) {
    controller_->embedder()->ShowUI();
  }
}

void SharePageHandler::Close() {
  // The bubble, or the join page's tab.
  if (controller_->embedder()) {
    controller_->embedder()->CloseUI();
  } else {
    web_ui_->GetWebContents()->ClosePage();
  }
}

void SharePageHandler::OnRoomsChanged() {
  page_->RoomsChanged();
}

}  // namespace compass
