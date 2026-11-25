// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/browser/avf_player_factory_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "compass/media/avf/common/avf_features.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/gpu_service_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/common/content_switches.h"
#include "mojo/public/cpp/bindings/message.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace compass::avf {
namespace {

// One AVFoundation utility process for all players, launched on first use.
mojom::AVFMediaService& GetMediaService() {
  static base::NoDestructor<mojo::Remote<mojom::AVFMediaService>> service;
  if (!service->is_bound() || !service->is_connected()) {
    service->reset();
    content::ServiceProcessHost::Launch(
        service->BindNewPipeAndPassReceiver(),
        content::ServiceProcessHost::Options()
            .WithDisplayName("Compass Media (AVFoundation)")
            // AVFoundation and CoreAnimation need a run loop on the main
            // thread.
            .WithExtraCommandLineSwitches({switches::kMessageLoopTypeUi})
            .Pass());
    service->reset_on_disconnect();
  }
  return *service->get();
}

mojom::ExternalLayerRegistry& GetExternalLayerRegistry() {
  static base::NoDestructor<mojo::Remote<mojom::ExternalLayerRegistry>>
      registry;
  if (!registry->is_bound() || !registry->is_connected()) {
    registry->reset();
    content::BindInterfaceInGpuProcess(registry->BindNewPipeAndPassReceiver());
    registry->reset_on_disconnect();
  }
  return *registry->get();
}

}  // namespace

// static
void AVFPlayerFactoryImpl::Create(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<mojom::AVFPlayerFactory> receiver) {
  if (!base::FeatureList::IsEnabled(kAVFoundationHls)) {
    return;
  }
  // DocumentService deletes itself with the document or the connection.
  new AVFPlayerFactoryImpl(*render_frame_host, std::move(receiver));
}

AVFPlayerFactoryImpl::AVFPlayerFactoryImpl(
    content::RenderFrameHost& render_frame_host,
    mojo::PendingReceiver<mojom::AVFPlayerFactory> receiver)
    : DocumentService(render_frame_host, std::move(receiver)) {}

AVFPlayerFactoryImpl::~AVFPlayerFactoryImpl() {
  for (const auto& id : overlay_plane_ids_) {
    GetExternalLayerRegistry().Unregister(id);
  }
}

void AVFPlayerFactoryImpl::CreatePlayer(
    const GURL& url,
    mojo::PendingReceiver<mojom::AVFPlayer> player,
    mojo::PendingRemote<mojom::AVFPlayerClient> client,
    CreatePlayerCallback callback) {
  if (!ShouldUseAVFoundation(url)) {
    mojo::ReportBadMessage("AVFPlayerFactory: not an HLS URL");
    return;
  }
  auto options = mojom::AVFPlayerOptions::New();
  options->user_agent = embedder_support::GetUserAgent();
  // Like a media element's request under the default referrer policy
  // (strict-origin-when-cross-origin): the origin only.
  url::Origin origin = render_frame_host().GetLastCommittedOrigin();
  if (!origin.opaque()) {
    options->referrer = origin.GetURL();
  }
  GetMediaService().CreatePlayer(
      url, std::move(options), std::move(player), std::move(client),
      base::BindOnce(&AVFPlayerFactoryImpl::OnPlayerCreated,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AVFPlayerFactoryImpl::OnPlayerCreated(CreatePlayerCallback callback,
                                           uint32_t ca_context_id) {
  if (!ca_context_id) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  base::UnguessableToken overlay_plane_id = base::UnguessableToken::Create();
  GetExternalLayerRegistry().Register(overlay_plane_id, ca_context_id);
  overlay_plane_ids_.push_back(overlay_plane_id);
  std::move(callback).Run(overlay_plane_id);
}

}  // namespace compass::avf
