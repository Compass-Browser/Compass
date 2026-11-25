// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_MEDIA_AVF_BROWSER_AVF_PLAYER_FACTORY_IMPL_H_
#define COMPASS_MEDIA_AVF_BROWSER_AVF_PLAYER_FACTORY_IMPL_H_

#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "compass/media/avf/public/mojom/avf_player.mojom.h"
#include "content/public/browser/document_service.h"
#include "net/cookies/canonical_cookie.h"

namespace content {
class RenderFrameHost;
}

namespace compass::avf {

// Lets a document create AVPlayers in the AVFoundation utility process, and
// tells the GPU process which CoreAnimation context to show for each player's
// overlay plane. One per document.
class AVFPlayerFactoryImpl
    : public content::DocumentService<mojom::AVFPlayerFactory> {
 public:
  static void Create(content::RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<mojom::AVFPlayerFactory> receiver);

  AVFPlayerFactoryImpl(const AVFPlayerFactoryImpl&) = delete;
  AVFPlayerFactoryImpl& operator=(const AVFPlayerFactoryImpl&) = delete;

  // mojom::AVFPlayerFactory:
  void CreatePlayer(const GURL& url,
                    mojo::PendingReceiver<mojom::AVFPlayer> player,
                    mojo::PendingRemote<mojom::AVFPlayerClient> client,
                    CreatePlayerCallback callback) override;
  void IsPresentingAsSafari(IsPresentingAsSafariCallback callback) override;
  void CreateCdm(CreateCdmCallback callback) override;

 private:
  AVFPlayerFactoryImpl(content::RenderFrameHost& render_frame_host,
                       mojo::PendingReceiver<mojom::AVFPlayerFactory> receiver);
  ~AVFPlayerFactoryImpl() override;

  void LaunchPlayer(const GURL& url,
                    mojo::PendingReceiver<mojom::AVFPlayer> player,
                    mojo::PendingRemote<mojom::AVFPlayerClient> client,
                    CreatePlayerCallback callback,
                    mojom::AVFPlayerOptionsPtr options,
                    const net::CookieAccessResultList& cookies);
  void OnPlayerCreated(CreatePlayerCallback callback, uint32_t ca_context_id);

  std::vector<base::UnguessableToken> overlay_plane_ids_;
  base::WeakPtrFactory<AVFPlayerFactoryImpl> weak_factory_{this};
};

}  // namespace compass::avf

#endif  // COMPASS_MEDIA_AVF_BROWSER_AVF_PLAYER_FACTORY_IMPL_H_
