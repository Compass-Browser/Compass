// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/live_folders_untrusted_ui.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/live_folders/live_plugin_package.h"
#include "compass/browser/ui/live_folders/live_folder_host.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

#if BUILDFLAG(IS_ANDROID)
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/webui_config_map.h"
#endif

namespace compass {

namespace {

constexpr std::string_view kInstalledPrefix = "installed/";

// The page's own files, out of Compass's WebUI resources.
bool IsLiveFoldersResource(std::string_view path) {
  return path == "live_folders.html" ||
         path == "live_folders.mojom-webui.js" ||
         path.starts_with("live_folders/") || path.starts_with("plugins/");
}

}  // namespace

#if BUILDFLAG(IS_ANDROID)
void RegisterLiveFoldersUntrustedUI() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  content::WebUIConfigMap::GetInstance().AddUntrustedWebUIConfig(
      std::make_unique<LiveFoldersUntrustedUIConfig>());
  content::WebUIBrowserInterfaceBrokerRegistry::GetUntrustedRegistry()
      .ForWebUI<LiveFoldersUntrustedUI>()
      .Add<live_folders::mojom::PageHandlerFactory>();
}
#endif  // BUILDFLAG(IS_ANDROID)

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

  // Installed plugins' files, from <user data>/Compass Plugins.
  Profile* profile = Profile::FromWebUI(web_ui);
  source->SetRequestFilter(
      base::BindRepeating([](const std::string& path) {
        return path.starts_with(kInstalledPrefix);
      }),
      base::BindRepeating(
          [](base::FilePath plugins_dir, const std::string& path,
             content::WebUIDataSource::GotDataCallback callback) {
            std::string_view relative =
                std::string_view(path).substr(kInstalledPrefix.size());
            relative = relative.substr(0, relative.find_first_of("?#"));
            const base::FilePath file =
                LivePluginFilePath(plugins_dir, relative);
            if (file.empty()) {
              std::move(callback).Run(nullptr);
              return;
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
                base::BindOnce(
                    [](base::FilePath file)
                        -> scoped_refptr<base::RefCountedMemory> {
                      std::string contents;
                      if (!base::ReadFileToString(file, &contents)) {
                        return nullptr;
                      }
                      return base::MakeRefCounted<base::RefCountedString>(
                          std::move(contents));
                    },
                    file),
                std::move(callback));
          },
          LivePluginsDir(profile->GetPath().DirName())));

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
