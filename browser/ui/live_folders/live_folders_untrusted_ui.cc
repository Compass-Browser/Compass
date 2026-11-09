// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/live_folders_untrusted_ui.h"

#include <string_view>
#include <utility>
#include <vector>

#include "chrome/browser/profiles/profile.h"
#include "compass/browser/ui/live_folders/live_folder_host.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace compass {

namespace {

// The page's own files, out of Compass's WebUI resources.
bool IsLiveFoldersResource(std::string_view path) {
  return path == "live_folders.html" ||
         path == "live_folders.mojom-webui.js" ||
         path.starts_with("live_folders/") || path.starts_with("plugins/");
}

}  // namespace

LiveFoldersUntrustedUIConfig::LiveFoldersUntrustedUIConfig()
    : DefaultWebUIConfig(content::kChromeUIUntrustedScheme, kLiveFoldersHost) {}

LiveFoldersUntrustedUI::LiveFoldersUntrustedUI(content::WebUI* web_ui)
    : ui::UntrustedWebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kLiveFoldersUrl);
  std::vector<webui::ResourcePath> resources;
  for (const webui::ResourcePath& resource : kCompassSidebarResources) {
    if (IsLiveFoldersResource(resource.path)) {
      resources.push_back(resource);
    }
  }
  source->AddResourcePaths(resources);
  source->SetDefaultResource(IDR_COMPASS_SIDEBAR_LIVE_FOLDERS_HTML);

  using Directive = network::mojom::CSPDirectiveName;
  source->OverrideContentSecurityPolicy(Directive::DefaultSrc,
                                        "default-src 'none';");
  // Its own scripts and the shared Mojo bindings; no eval anywhere.
  source->OverrideContentSecurityPolicy(
      Directive::ScriptSrc,
      "script-src 'self' chrome-untrusted://resources;");
  source->OverrideContentSecurityPolicy(Directive::WorkerSrc,
                                        "worker-src 'self';");
  // Plugins reach the network only through compass.fetch().
  source->OverrideContentSecurityPolicy(Directive::ConnectSrc,
                                        "connect-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::ImgSrc, "img-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::StyleSrc,
                                        "style-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::FrameAncestors,
                                        "frame-ancestors 'none';");
  // Workers and importScripts() take plain URLs.
  source->DisableTrustedTypesCSP();
}

LiveFoldersUntrustedUI::~LiveFoldersUntrustedUI() = default;

void LiveFoldersUntrustedUI::BindInterface(
    mojo::PendingReceiver<live_folders::mojom::PageHandlerFactory> receiver) {
  factory_receiver_.reset();
  factory_receiver_.Bind(std::move(receiver));
}

void LiveFoldersUntrustedUI::CreatePageHandler(
    mojo::PendingRemote<live_folders::mojom::Page> page,
    mojo::PendingReceiver<live_folders::mojom::PageHandler> handler) {
  content::WebContents* contents = web_ui()->GetWebContents();
  LiveFolderService* service = LiveFolderService::Get(
      Profile::FromBrowserContext(contents->GetBrowserContext()));
  // Opened anywhere else (typed into a tab, say), the page gets nothing.
  if (!service || !service->host().IsHostContents(contents)) {
    return;
  }
  service->host().BindPage(std::move(page), std::move(handler));
}

WEB_UI_CONTROLLER_TYPE_IMPL(LiveFoldersUntrustedUI)

}  // namespace compass
