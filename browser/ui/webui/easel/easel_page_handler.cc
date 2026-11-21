// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/room_service.h"
#include "compass/browser/ui/webui/easel/easel_page_handler.h"

#include <utility>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/ui/easels/easel_refresh.h"
#include "compass/browser/ui/webui/easel/easel_conversions.h"
#include "compass/browser/ui/webui/easel/link_card_fetch.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass::easel {
namespace {

bool IsWebUrl(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS();
}

}  // namespace

EaselPageHandler::EaselPageHandler(
    mojo::PendingReceiver<mojom::PageHandler> receiver,
    mojo::PendingRemote<mojom::Page> page,
    Profile* profile,
    content::WebContents* contents)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      profile_(profile),
      contents_(contents),
      store_(easels::EaselStore::Get(profile)) {
  if (store_) {
    observation_.Observe(store_.get());
  }
}

EaselPageHandler::~EaselPageHandler() = default;

void EaselPageHandler::GetEasels(GetEaselsCallback callback) {
  if (!store_) {
    std::move(callback).Run(false, {});
    return;
  }
  if (!store_->loaded()) {
    store_->WhenLoaded(base::BindOnce(&EaselPageHandler::GetEasels,
                                      weak_factory_.GetWeakPtr(),
                                      std::move(callback)));
    return;
  }
  std::vector<mojom::EaselSummaryPtr> easels;
  for (const sync_pb::CompassEasel* easel : store_->Easels()) {
    easels.push_back(ToMojo(
        *easel, static_cast<int>(store_->ObjectsOf(easel->uuid()).size())));
  }
  std::move(callback).Run(true, std::move(easels));
}

void EaselPageHandler::GetEasel(const std::string& uuid,
                                GetEaselCallback callback) {
  if (!store_) {
    std::move(callback).Run(mojom::EaselState::kUnavailable, nullptr, {});
    return;
  }
  if (!store_->loaded()) {
    store_->WhenLoaded(base::BindOnce(&EaselPageHandler::GetEasel,
                                      weak_factory_.GetWeakPtr(), uuid,
                                      std::move(callback)));
    return;
  }
  const sync_pb::CompassEasel* easel = store_->FindEasel(uuid);
  if (!easel) {
    std::move(callback).Run(mojom::EaselState::kNotFound, nullptr, {});
    return;
  }
  std::vector<mojom::EaselObjectPtr> objects;
  for (const sync_pb::CompassEaselObject* object : store_->ObjectsOf(uuid)) {
    objects.push_back(ToMojo(*object));
  }
  mojom::EaselPtr result = ToMojoEasel(*easel);
  result->view_only = IsViewOnly(uuid);
  std::move(callback).Run(mojom::EaselState::kOk, std::move(result),
                          std::move(objects));
}

void EaselPageHandler::CreateEasel(const std::string& title,
                                   CreateEaselCallback callback) {
  if (!store_) {
    std::move(callback).Run(std::string(),
                            "Easels aren't available in this window.");
    return;
  }
  base::AutoReset<bool> changing(&changing_, true);
  base::expected<sync_pb::CompassEasel, std::string> easel =
      store_->CreateEasel(title);
  if (!easel.has_value()) {
    std::move(callback).Run(std::string(), easel.error());
    return;
  }
  std::move(callback).Run(easel->uuid(), std::string());
}

bool EaselPageHandler::IsViewOnly(const std::string& easel_uuid) const {
  // A viewer's shared easel (docs/design/sync.md, "Rooms: clients").
  rooms::RoomService* rooms =
      profile_->IsOffTheRecord() ? nullptr
                                 : rooms::RoomService::GetIfExists(profile_);
  return rooms && rooms->GetEaselState(easel_uuid).read_only;
}

void EaselPageHandler::RenameEasel(const std::string& uuid,
                                   const std::string& title) {
  if (store_ && !IsViewOnly(uuid)) {
    base::AutoReset<bool> changing(&changing_, true);
    store_->RenameEasel(uuid, title);
  }
}

void EaselPageHandler::SetPalette(const std::string& uuid,
                                  const std::string& palette) {
  if (store_ && !IsViewOnly(uuid)) {
    base::AutoReset<bool> changing(&changing_, true);
    store_->SetPalette(uuid, palette);
  }
}

void EaselPageHandler::DeleteEasel(const std::string& uuid) {
  if (store_ && !IsViewOnly(uuid)) {
    base::AutoReset<bool> changing(&changing_, true);
    store_->DeleteEasel(uuid);
  }
}

void EaselPageHandler::SaveObjects(const std::string& easel_uuid,
                                   std::vector<mojom::EaselObjectPtr> objects,
                                   SaveObjectsCallback callback) {
  if (!store_) {
    std::move(callback).Run("Easels aren't available in this window.", {});
    return;
  }
  if (IsViewOnly(easel_uuid)) {
    std::move(callback).Run("You can only view this shared easel.", {});
    return;
  }
  std::vector<sync_pb::CompassEaselObject> protos;
  for (const mojom::EaselObjectPtr& object : objects) {
    const sync_pb::CompassEaselObject* existing =
        store_->FindObject(object->uuid);
    if (existing && existing->easel_uuid() != easel_uuid) {
      std::move(callback).Run("The object is on another Easel.", {});
      return;
    }
    base::expected<sync_pb::CompassEaselObject, std::string> proto =
        FromMojo(*object, easel_uuid, existing);
    if (!proto.has_value()) {
      std::move(callback).Run(proto.error(), {});
      return;
    }
    protos.push_back(std::move(*proto));
  }
  base::AutoReset<bool> changing(&changing_, true);
  base::expected<std::vector<sync_pb::CompassEaselObject>, std::string> saved =
      store_->SaveObjects(std::move(protos));
  if (!saved.has_value()) {
    std::move(callback).Run(saved.error(), {});
    return;
  }
  std::vector<mojom::EaselObjectPtr> result;
  for (const sync_pb::CompassEaselObject& object : *saved) {
    result.push_back(ToMojo(object));
  }
  std::move(callback).Run(std::string(), std::move(result));
}

void EaselPageHandler::DeleteObjects(const std::vector<std::string>& uuids) {
  if (!store_) {
    return;
  }
  base::AutoReset<bool> changing(&changing_, true);
  for (const std::string& uuid : uuids) {
    const sync_pb::CompassEaselObject* object = store_->FindObject(uuid);
    if (object && IsViewOnly(object->easel_uuid())) {
      continue;
    }
    store_->DeleteObject(uuid);
  }
}

void EaselPageHandler::AddImage(const std::vector<uint8_t>& data,
                                AddImageCallback callback) {
  if (!store_) {
    std::move(callback).Run("Easels aren't available in this window.",
                            std::string(), 0, 0);
    return;
  }
  easels::PrepareImage(
      std::string(data.begin(), data.end()),
      base::BindOnce(&EaselPageHandler::OnImagePrepared,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void EaselPageHandler::OnImagePrepared(
    AddImageCallback callback,
    base::expected<easels::PreparedImage, std::string> image) {
  if (!image.has_value()) {
    std::move(callback).Run(image.error(), std::string(), 0, 0);
    return;
  }
  const int width = image->width;
  const int height = image->height;
  base::AutoReset<bool> changing(&changing_, true);
  base::expected<std::string, std::string> sha256 =
      store_->AddAsset(std::move(image->data), std::move(image->mime_type));
  if (!sha256.has_value()) {
    std::move(callback).Run(sha256.error(), std::string(), 0, 0);
    return;
  }
  std::move(callback).Run(std::string(), *sha256, width, height);
}

void EaselPageHandler::AddLink(const std::string& url,
                               AddLinkCallback callback) {
  const GURL gurl(url);
  if (!IsWebUrl(gurl)) {
    std::move(callback).Run(nullptr);
    return;
  }
  const int id = ++last_link_fetch_id_;
  auto fetch = std::make_unique<LinkCardFetch>(
      gurl,
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      store_ ? store_->GetWeakPtr()
             : base::WeakPtr<easels::EaselStore>(),
      base::BindOnce(&EaselPageHandler::OnLinkFetched,
                     weak_factory_.GetWeakPtr(), id, std::move(callback)));
  LinkCardFetch* raw = fetch.get();
  link_fetches_[id] = std::move(fetch);
  raw->Start();
}

void EaselPageHandler::OnLinkFetched(int id,
                                     AddLinkCallback callback,
                                     mojom::LinkContentPtr link) {
  link_fetches_.erase(id);
  std::move(callback).Run(std::move(link));
}

void EaselPageHandler::OpenUrl(const std::string& url) {
  const GURL gurl(url);
  if (!IsWebUrl(gurl) || !contents_) {
    return;
  }
  // The tab's delegate places it: on the desktop a tab of this window (its
  // space), on Android a tab next to this one.
  contents_->OpenURL(
      content::OpenURLParams(gurl, content::Referrer(),
                             WindowOpenDisposition::NEW_FOREGROUND_TAB,
                             ui::PAGE_TRANSITION_LINK,
                             /*is_renderer_initiated=*/false),
      /*navigation_handle_callback=*/{});
}

void EaselPageHandler::RefreshCapture(const std::string& object_uuid,
                                      RefreshCaptureCallback callback) {
  easels::RefreshCapture(
      profile_, object_uuid,
      base::BindOnce(
          [](RefreshCaptureCallback callback, std::string error) {
            std::move(callback).Run(error);
          },
          std::move(callback)));
}

void EaselPageHandler::OnEaselChanged(const std::string& easel_uuid) {
  if (!changing_) {
    page_->OnEaselChanged(easel_uuid);
  }
}

}  // namespace compass::easel
