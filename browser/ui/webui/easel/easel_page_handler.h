// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_PAGE_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_PAGE_HANDLER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/types/expected.h"
#include "compass/browser/easels/easel_images.h"
#include "compass/browser/easels/easel_store.h"
#include "compass/browser/ui/webui/easel/easel.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace compass::easel {

class LinkCardFetch;

// chrome://easel's browser side: the page's requests on the profile's
// EaselStore, and the store's changes pushed to the page.
class EaselPageHandler : public mojom::PageHandler,
                         public easels::EaselStore::Observer {
 public:
  // |contents| is the page's own tab. The profile may have no store
  // (Incognito, Guest): the page then says Easels aren't available.
  EaselPageHandler(mojo::PendingReceiver<mojom::PageHandler> receiver,
                   mojo::PendingRemote<mojom::Page> page,
                   Profile* profile,
                   content::WebContents* contents);
  EaselPageHandler(const EaselPageHandler&) = delete;
  EaselPageHandler& operator=(const EaselPageHandler&) = delete;
  ~EaselPageHandler() override;

  // mojom::PageHandler:
  void GetEasels(GetEaselsCallback callback) override;
  void GetEasel(const std::string& uuid, GetEaselCallback callback) override;
  void CreateEasel(const std::string& title,
                   CreateEaselCallback callback) override;
  void RenameEasel(const std::string& uuid, const std::string& title) override;
  void SetPalette(const std::string& uuid, const std::string& palette) override;
  void DeleteEasel(const std::string& uuid) override;
  void SaveObjects(const std::string& easel_uuid,
                   std::vector<mojom::EaselObjectPtr> objects,
                   SaveObjectsCallback callback) override;
  void DeleteObjects(const std::vector<std::string>& uuids) override;
  void AddImage(const std::vector<uint8_t>& data,
                AddImageCallback callback) override;
  void AddLink(const std::string& url, AddLinkCallback callback) override;
  void OpenUrl(const std::string& url) override;
  void RefreshCapture(const std::string& object_uuid,
                      RefreshCaptureCallback callback) override;

  // easels::EaselStore::Observer:
  void OnEaselChanged(const std::string& easel_uuid) override;

 private:
  // A viewer's shared easel: edits are refused.
  bool IsViewOnly(const std::string& easel_uuid) const;
  void OnImagePrepared(
      AddImageCallback callback,
      base::expected<easels::PreparedImage, std::string> image);
  void OnLinkFetched(int id,
                     AddLinkCallback callback,
                     mojom::LinkContentPtr link);

  mojo::Receiver<mojom::PageHandler> receiver_;
  mojo::Remote<mojom::Page> page_;
  const raw_ptr<Profile> profile_;
  const raw_ptr<content::WebContents> contents_;
  // Null without Easels.
  const raw_ptr<easels::EaselStore> store_;
  // While this page changes the store: its own edits aren't echoed back.
  bool changing_ = false;
  // AddLink()'s, by id.
  std::map<int, std::unique_ptr<LinkCardFetch>> link_fetches_;
  int last_link_fetch_id_ = 0;
  base::ScopedObservation<easels::EaselStore, easels::EaselStore::Observer>
      observation_{this};
  base::WeakPtrFactory<EaselPageHandler> weak_factory_{this};
};

}  // namespace compass::easel

#endif  // COMPASS_BROWSER_UI_WEBUI_EASEL_EASEL_PAGE_HANDLER_H_
