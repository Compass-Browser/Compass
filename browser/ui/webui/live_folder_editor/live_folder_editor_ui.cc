// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/live_folder_editor/live_folder_editor_ui.h"

#include <map>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "compass/browser/ui/webui/bubble/bubble_look_mojo.h"
#include "compass/browser/ui/webui/localized_strings/localized_strings.h"
#include "compass/browser/ui/webui/sidebar/live_plugin_mojo.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "compass/strings/grit/compass_strings.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "net/base/url_util.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/webui/webui_util.h"

namespace compass {

GURL LiveFolderEditorUrl(const std::optional<std::string>& parent_id,
                         const std::optional<std::string>& folder_id) {
  std::string query;
  if (folder_id) {
    query = base::StrCat(
        {"?folder=", base::EscapeQueryParamValue(*folder_id, false)});
  } else if (parent_id) {
    query = base::StrCat(
        {"?parent=", base::EscapeQueryParamValue(*parent_id, false)});
  }
  return GURL(base::StrCat({content::kChromeUIScheme, "://",
                            kCompassLiveFolderEditorHost, "/", query}));
}

// The UI ----------------------------------------------------------------------

CompassLiveFolderEditorUIConfig::CompassLiveFolderEditorUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  kCompassLiveFolderEditorHost) {}

bool CompassLiveFolderEditorUIConfig::ShouldAutoResizeHost() {
  // The bubble grows and shrinks with the editor: the plugin list, a
  // plugin's form, the install review.
  return true;
}

CompassLiveFolderEditorUI::CompassLiveFolderEditorUI(content::WebUI* web_ui,
                                                     const GURL& url)
    : TopChromeWebUIController(web_ui) {
  std::string value;
  if (net::GetValueForKeyInQuery(url, "folder", &value) && !value.empty()) {
    folder_id_ = value;
  } else if (net::GetValueForKeyInQuery(url, "parent", &value) &&
             !value.empty()) {
    parent_id_ = value;
  }
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      profile, kCompassLiveFolderEditorHost);
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_LIVE_FOLDER_EDITOR_HTML);
  AddSidebarPakLocalizedStrings(source);
  // Plugin icons are data: URLs.
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ImgSrc,
      "img-src chrome://resources chrome://theme data: 'self';");
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassLiveFolderEditorUI::~CompassLiveFolderEditorUI() = default;

void CompassLiveFolderEditorUI::BindInterface(
    mojo::PendingReceiver<live_folder_editor::mojom::PageHandlerFactory>
        receiver) {
  factory_receiver_.reset();
  factory_receiver_.Bind(std::move(receiver));
}

void CompassLiveFolderEditorUI::CreatePageHandler(
    mojo::PendingReceiver<live_folder_editor::mojom::PageHandler> receiver) {
  // Only in a window's bubble (ShowLiveFolderEditorBubble()): the window
  // says whose space the folder is in.
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  if (!browser || !WindowFeatures::From(browser)) {
    return;
  }
  handler_ = std::make_unique<LiveFolderEditorPageHandler>(std::move(receiver),
                                                           this, browser);
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassLiveFolderEditorUI)

// The handler -----------------------------------------------------------------

namespace {

sidebar::mojom::LivePluginPtr ToMojoPlugin(const LivePlugin& plugin,
                                           const LiveFolderStore* store) {
  const LivePluginManifest& manifest = plugin.manifest;
  auto result = sidebar::mojom::LivePlugin::New();
  result->id = manifest.id;
  result->name = manifest.name;
  result->description = manifest.description;
  result->icon_url = plugin.IconUrl();
  result->config = ToMojoConfigFields(manifest.config);
  for (const auto& secret : manifest.secrets) {
    result->secrets.push_back(sidebar::mojom::LiveSecretField::New(
        secret.key, secret.label, secret.help, secret.optional,
        store && store->HasSecret(manifest.id, secret.key)));
  }
  // The manifest's patterns; placeholders show as the field they name.
  result->cookie_hosts = manifest.cookies;
  result->builtin = plugin.builtin;
  result->local = plugin.local;
  return result;
}

}  // namespace

LiveFolderEditorPageHandler::LiveFolderEditorPageHandler(
    mojo::PendingReceiver<live_folder_editor::mojom::PageHandler> receiver,
    CompassLiveFolderEditorUI* controller,
    BrowserWindowInterface* browser)
    : receiver_(this, std::move(receiver)),
      controller_(controller),
      browser_(browser) {}

LiveFolderEditorPageHandler::~LiveFolderEditorPageHandler() = default;

void LiveFolderEditorPageHandler::GetTarget(GetTargetCallback callback) {
  std::move(callback).Run(controller_->parent_id(), controller_->folder_id());
}

void LiveFolderEditorPageHandler::GetLivePlugins(
    GetLivePluginsCallback callback) {
  LiveFolderService* service = this->service();
  if (!service) {
    std::move(callback).Run({});
    return;
  }
  // Local plugins change on disk: read them again each time.
  service->RescanPlugins(base::BindOnce(
      [](base::WeakPtr<LiveFolderEditorPageHandler> handler,
         GetLivePluginsCallback callback) {
        std::vector<sidebar::mojom::LivePluginPtr> plugins;
        LiveFolderService* service = handler ? handler->service() : nullptr;
        if (service) {
          for (const LivePlugin& plugin : service->registry().plugins()) {
            // Preview-only plugins (compass.gmail) can't fill a folder;
            // Settings › Plugins lists them.
            if (!plugin.manifest.folder) {
              continue;
            }
            plugins.push_back(ToMojoPlugin(plugin, &service->store()));
          }
        }
        std::move(callback).Run(std::move(plugins));
      },
      weak_factory_.GetWeakPtr(), std::move(callback)));
}

void LiveFolderEditorPageHandler::GetLiveFolderSource(
    const std::string& id,
    GetLiveFolderSourceCallback callback) {
  SidebarNode* node = FindNode(id);
  LiveFolderService* service = this->service();
  if (!node || !node->is_live_folder() || !service) {
    std::move(callback).Run(nullptr);
    return;
  }
  const LiveSource& source = *node->live_source();
  std::move(callback).Run(sidebar::mojom::LiveFolderSource::New(
      source.plugin_id, base::UTF16ToUTF8(node->title()),
      base::WriteJson(source.config).value_or("{}"),
      service->store().CookiesGranted(node->id()),
      source.plugin_url.is_valid() ? source.plugin_url.spec() : std::string(),
      source.plugin_sha256));
}

void LiveFolderEditorPageHandler::SaveLiveFolder(
    sidebar::mojom::LiveFolderEditPtr edit,
    SaveLiveFolderCallback callback) {
  LiveFolderService* service = this->service();
  if (!service) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_LIVE_FOLDERS_UNAVAILABLE));
    return;
  }
  std::optional<base::Value> config =
      base::JSONReader::Read(edit->config_json, base::JSON_PARSE_RFC);
  if (!config || !config->is_dict()) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_LIVE_FOLDERS_SETTINGS_UNREADABLE));
    return;
  }
  std::map<std::string, std::string> secrets(edit->secrets.begin(),
                                             edit->secrets.end());
  if (edit->folder_id) {
    SidebarNode* folder = FindNode(*edit->folder_id);
    auto result = service->EditLiveFolder(folder, std::move(config->GetDict()),
                                          secrets, edit->grant_cookies);
    std::move(callback).Run(result.has_value() ? std::string()
                                               : result.error());
    return;
  }
  SpaceModel& space = space_model();
  SidebarNode* parent =
      edit->parent_id ? FindNode(*edit->parent_id) : space.pinned_root();
  if (!parent || !space.Contains(parent)) {
    parent = space.pinned_root();
  }
  auto result = service->CreateLiveFolder(
      space, parent, parent->children().size(), base::UTF8ToUTF16(edit->title),
      edit->plugin_id, std::move(config->GetDict()), secrets,
      edit->grant_cookies);
  std::move(callback).Run(result.has_value() ? std::string() : result.error());
}

void LiveFolderEditorPageHandler::PreviewLivePlugin(
    const std::string& url,
    PreviewLivePluginCallback callback) {
  LiveFolderService* service = this->service();
  if (!service) {
    std::move(callback).Run(nullptr, l10n_util::GetStringUTF8(
                                         IDS_COMPASS_LIVE_FOLDERS_UNAVAILABLE));
    return;
  }
  service->PreviewPlugin(
      GURL(url),
      base::BindOnce(
          [](PreviewLivePluginCallback callback,
             base::expected<LivePluginPackage, std::string> package) {
            if (!package.has_value()) {
              std::move(callback).Run(nullptr, package.error());
              return;
            }
            std::move(callback).Run(ToMojoPluginPreview(*package),
                                    std::string());
          },
          std::move(callback)));
}

void LiveFolderEditorPageHandler::InstallLivePlugin(
    const std::string& url,
    const std::string& sha256,
    InstallLivePluginCallback callback) {
  LiveFolderService* service = this->service();
  if (!service) {
    std::move(callback).Run(
        l10n_util::GetStringUTF8(IDS_COMPASS_LIVE_FOLDERS_UNAVAILABLE));
    return;
  }
  service->InstallPlugin(GURL(url), sha256, std::move(callback));
}

void LiveFolderEditorPageHandler::RemoveLivePlugin(
    const std::string& id,
    RemoveLivePluginCallback callback) {
  LiveFolderService* service = this->service();
  if (!service) {
    std::move(callback).Run();
    return;
  }
  service->RemovePlugin(id, std::move(callback));
}

void LiveFolderEditorPageHandler::OpenLivePluginsFolder() {
  if (LiveFolderService* service = this->service()) {
    service->OpenPluginsFolder();
  }
}

void LiveFolderEditorPageHandler::GetLook(GetLookCallback callback) {
  std::move(callback).Run(
      GetBubbleLookFor(controller_->web_ui()->GetWebContents()));
}

void LiveFolderEditorPageHandler::ShowUI() {
  if (controller_->embedder()) {
    controller_->embedder()->ShowUI();
  }
}

void LiveFolderEditorPageHandler::Close() {
  if (controller_->embedder()) {
    controller_->embedder()->CloseUI();
  }
}

Profile* LiveFolderEditorPageHandler::profile() {
  return browser_->GetProfile();
}

LiveFolderService* LiveFolderEditorPageHandler::service() {
  return LiveFolderService::Get(profile());
}

SpaceModel& LiveFolderEditorPageHandler::space_model() {
  return WindowFeatures::From(browser_)->space_model();
}

SpaceModel& LiveFolderEditorPageHandler::favorites() {
  return WindowFeatures::From(browser_)->service().space_list().favorites();
}

SidebarNode* LiveFolderEditorPageHandler::FindNode(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (!uuid.is_valid()) {
    return nullptr;
  }
  SidebarNode* node = space_model().FindById(uuid);
  return node ? node : favorites().FindById(uuid);
}

}  // namespace compass
