// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/media/avf/browser/avf_player_factory_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/content_settings/cookie_settings_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/media/avf/common/avf_features.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/gpu_service_registry.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "mojo/public/cpp/bindings/message.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/cookies/cookie_setting_override.h"
#include "net/cookies/cookie_util.h"
#include "net/cookies/site_for_cookies.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

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
  content::RenderFrameHost& frame = render_frame_host();
  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(&frame);

  auto options = mojom::AVFPlayerOptions::New();
  options->user_agent = embedder_support::GetUserAgent();
  if (web_contents) {
    content::NavigationEntry* entry =
        web_contents->GetController().GetLastCommittedEntry();
    const std::string& ua_override =
        web_contents->GetUserAgentOverride().ua_string_override;
    if (entry && entry->GetIsOverridingUserAgent() && !ua_override.empty()) {
      options->user_agent = ua_override;
    }
  }
  // Like a media element's request under the default referrer policy
  // (strict-origin-when-cross-origin): the origin only.
  url::Origin origin = frame.GetLastCommittedOrigin();
  if (!origin.opaque()) {
    options->referrer = origin.GetURL();
  }

  auto create = base::BindOnce(&AVFPlayerFactoryImpl::LaunchPlayer,
                               weak_factory_.GetWeakPtr(), url,
                               std::move(player), std::move(client),
                               std::move(callback));

  // Cookies, like the media element's own request would send them: only for
  // a stream that is first-party to the frame (third-party cookie rules
  // aren't applied outside the network service), and only where the
  // profile allows cookies.
  const blink::StorageKey& storage_key = frame.GetStorageKey();
  net::SiteForCookies site_for_cookies = storage_key.ToNetSiteForCookies();
  url::Origin top_frame_origin = frame.GetMainFrame()->GetLastCommittedOrigin();
  scoped_refptr<content_settings::CookieSettings> cookie_settings =
      CookieSettingsFactory::GetForProfile(
          Profile::FromBrowserContext(frame.GetBrowserContext()));
  bool allowed = site_for_cookies.IsFirstParty(url) && cookie_settings &&
                 cookie_settings->IsFullCookieAccessAllowed(
                     url, site_for_cookies, top_frame_origin,
                     net::CookieSettingOverrides(), std::nullopt);
  if (!allowed) {
    std::move(create).Run(std::move(options), {});
    return;
  }
  net::CookieOptions cookie_options;
  cookie_options.set_include_httponly();
  cookie_options.set_same_site_cookie_context(
      net::cookie_util::ComputeSameSiteContextForSubresource(
          url, site_for_cookies, /*force_ignore_site_for_cookies=*/false));
  frame.GetStoragePartition()->GetCookieManagerForBrowserProcess()->GetCookieList(
      url, cookie_options, net::CookiePartitionKeyCollection(),
      base::BindOnce(
          [](base::OnceCallback<void(mojom::AVFPlayerOptionsPtr,
                                     const net::CookieAccessResultList&)>
                 create,
             mojom::AVFPlayerOptionsPtr options,
             const net::CookieAccessResultList& cookies,
             const net::CookieAccessResultList& excluded) {
            std::move(create).Run(std::move(options), cookies);
          },
          std::move(create), std::move(options)));
}

void AVFPlayerFactoryImpl::LaunchPlayer(
    const GURL& url,
    mojo::PendingReceiver<mojom::AVFPlayer> player,
    mojo::PendingRemote<mojom::AVFPlayerClient> client,
    CreatePlayerCallback callback,
    mojom::AVFPlayerOptionsPtr options,
    const net::CookieAccessResultList& cookies) {
  for (const auto& cookie_with_access : cookies) {
    const net::CanonicalCookie& cookie = cookie_with_access.cookie;
    auto avf_cookie = mojom::AVFCookie::New();
    avf_cookie->name = cookie.Name();
    avf_cookie->value = cookie.Value();
    avf_cookie->domain = cookie.Domain();
    avf_cookie->path = cookie.Path();
    avf_cookie->secure = cookie.SecureAttribute();
    avf_cookie->http_only = cookie.IsHttpOnly();
    if (cookie.IsPersistent()) {
      avf_cookie->expiry = cookie.ExpiryDate();
    }
    options->cookies.push_back(std::move(avf_cookie));
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
