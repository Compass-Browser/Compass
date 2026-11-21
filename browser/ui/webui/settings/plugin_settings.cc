// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Settings › Plugins (compass://settings/plugins): every live folder and tab
// preview plugin, and the user's settings for its tab preview
// (docs/design/settings.md, "Plugins"; docs/design/live-folders.md, "Tab
// previews"). Part of ShortcutsPageHandler, the settings pages' handler.

#include <string>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/ui/views/claude/claude_panel.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/live_folders/live_plugin_package.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/ui/live_folders/live_folder_service.h"
#include "compass/browser/ui/live_folders/live_plugin_registry.h"
#include "compass/browser/ui/live_folders/tab_preview_service.h"
#include "compass/browser/ui/webui/settings/compass_settings_ui.h"
#include "compass/browser/ui/webui/sidebar/live_plugin_mojo.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "components/prefs/pref_service.h"
#include "ui/base/resource/resource_bundle.h"

namespace compass {

namespace {

constexpr std::string_view kInstalledPrefix = "installed/";

std::string BuiltinFile(const std::string& path) {
  return LivePluginRegistry::ReadBuiltinFile(path);
}

std::string IconDataUrl(const LivePlugin& plugin) {
  if (!plugin.icon_data_url.empty()) {
    return plugin.icon_data_url;
  }
  if (!plugin.builtin || plugin.manifest.icon.empty()) {
    return std::string();
  }
  const std::string svg = BuiltinFile(plugin.base_path + plugin.manifest.icon);
  return svg.empty() ? std::string()
                     : "data:image/svg+xml;base64," + base::Base64Encode(svg);
}

std::string SourceOf(const LivePlugin& plugin) {
  if (plugin.builtin) {
    return "builtin";
  }
  if (plugin.local) {
    return plugin.manifest.id.starts_with("claude.") ? "claude" : "local";
  }
  return "installed";
}

sidebar::mojom::PluginSettingsPtr ToSettings(const LivePlugin& plugin,
                                             LiveFolderService& service) {
  const LivePluginManifest& manifest = plugin.manifest;
  TabPreviewService& previews = service.previews();
  auto settings = sidebar::mojom::PluginSettings::New();
  settings->id = manifest.id;
  settings->name = manifest.name;
  settings->version = manifest.version;
  settings->description = manifest.description;
  settings->icon_url = IconDataUrl(plugin);
  settings->source = SourceOf(plugin);
  settings->source_url =
      plugin.source.is_valid() ? plugin.source.spec() : std::string();
  settings->folder = manifest.folder;
  settings->folder_count =
      static_cast<int32_t>(service.CountFoldersUsing(manifest.id));
  settings->hosts = manifest.hosts;
  settings->cookie_hosts = manifest.cookies;
  settings->preview = manifest.preview.has_value();
  if (manifest.preview) {
    settings->preview_sites = PreviewSites(*manifest.preview);
    settings->preview_enabled = previews.IsEnabled(plugin);
    settings->badge = manifest.preview->badge;
    if (manifest.preview->session) {
      settings->session_site = manifest.preview->session->site;
      settings->related_hosts = manifest.preview->session->related;
      settings->session_granted = previews.IsSessionGranted(plugin);
    }
    settings->preview_config = ToMojoConfigFields(manifest.preview->config);
    settings->preview_config_json =
        base::WriteJson(previews.GetConfig(manifest.id)).value_or("{}");
    settings->log = previews.GetLog(manifest.id);
  }
  return settings;
}

}  // namespace

void ShortcutsPageHandler::GetPlugins(GetPluginsCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(profile_);
  std::vector<sidebar::mojom::PluginSettingsPtr> plugins;
  if (service) {
    for (const LivePlugin& plugin : service->registry().plugins()) {
      plugins.push_back(ToSettings(plugin, *service));
    }
  }
  std::move(callback).Run(
      profile_->GetPrefs()->GetBoolean(prefs::kTabPreviewsEnabled),
      std::move(plugins),
      profile_->GetPrefs()->GetBoolean(claude::prefs::kPlugins));
}

void ShortcutsPageHandler::SetTabPreviewsEnabled(
    bool enabled,
    SetTabPreviewsEnabledCallback callback) {
  profile_->GetPrefs()->SetBoolean(prefs::kTabPreviewsEnabled, enabled);
  if (LiveFolderService* service = LiveFolderService::Get(profile_)) {
    service->previews().ClearCache();
  }
  std::move(callback).Run();
}

void ShortcutsPageHandler::SetPluginPreviewEnabled(
    const std::string& id,
    bool enabled,
    SetPluginPreviewEnabledCallback callback) {
  if (LiveFolderService* service = LiveFolderService::Get(profile_)) {
    service->previews().SetEnabled(id, enabled);
  }
  std::move(callback).Run();
}

void ShortcutsPageHandler::SetPluginSessionGranted(
    const std::string& id,
    bool granted,
    SetPluginSessionGrantedCallback callback) {
  if (LiveFolderService* service = LiveFolderService::Get(profile_)) {
    service->previews().SetSessionGranted(id, granted);
  }
  std::move(callback).Run();
}

void ShortcutsPageHandler::SetPluginPreviewConfig(
    const std::string& id,
    const std::string& config_json,
    SetPluginPreviewConfigCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(profile_);
  const LivePlugin* plugin = service ? service->registry().Find(id) : nullptr;
  if (!plugin || !plugin->manifest.preview) {
    std::move(callback).Run("There's no such plugin.");
    return;
  }
  std::optional<base::Value> config =
      base::JSONReader::Read(config_json, base::JSON_PARSE_RFC);
  if (!config || !config->is_dict()) {
    std::move(callback).Run("The settings aren't valid.");
    return;
  }
  auto validated =
      ValidateLiveConfigFields(plugin->manifest.preview->config,
                               config->GetDict());
  if (!validated.has_value()) {
    std::move(callback).Run(base::JoinString(validated.error(), ". "));
    return;
  }
  // The user's values, not the defaults: a plugin update's new defaults apply.
  base::DictValue kept;
  for (const auto& field : plugin->manifest.preview->config) {
    if (const base::Value* value = config->GetDict().Find(field.key)) {
      kept.Set(field.key, value->Clone());
    }
  }
  service->previews().SetConfig(id, std::move(kept));
  std::move(callback).Run(std::string());
}

void ShortcutsPageHandler::GetPluginSource(const std::string& id,
                                           GetPluginSourceCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(profile_);
  const LivePlugin* plugin = service ? service->registry().Find(id) : nullptr;
  if (!plugin) {
    std::move(callback).Run(std::string(), std::string());
    return;
  }
  if (plugin->builtin) {
    std::move(callback).Run(BuiltinFile(plugin->base_path + "manifest.json"),
                            BuiltinFile(plugin->base_path +
                                        plugin->manifest.entry));
    return;
  }
  // Installed and local plugins: from <user data>/Compass Plugins.
  std::string_view relative = plugin->base_path;
  if (relative.starts_with(kInstalledPrefix)) {
    relative.remove_prefix(kInstalledPrefix.size());
  }
  const base::FilePath manifest_path = LivePluginFilePath(
      service->plugins_dir(), std::string(relative) + "manifest.json");
  const base::FilePath entry_path = LivePluginFilePath(
      service->plugins_dir(), std::string(relative) + plugin->manifest.entry);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath manifest, base::FilePath entry) {
            std::pair<std::string, std::string> files;
            if (!manifest.empty()) {
              base::ReadFileToString(manifest, &files.first);
            }
            if (!entry.empty()) {
              base::ReadFileToString(entry, &files.second);
            }
            return files;
          },
          manifest_path, entry_path),
      base::BindOnce(
          [](GetPluginSourceCallback callback,
             std::pair<std::string, std::string> files) {
            std::move(callback).Run(std::move(files.first),
                                    std::move(files.second));
          },
          std::move(callback)));
}

void ShortcutsPageHandler::PreviewPlugin(const std::string& url,
                                         PreviewPluginCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(profile_);
  if (!service) {
    std::move(callback).Run(nullptr, "Plugins aren't available here.");
    return;
  }
  service->PreviewPlugin(
      GURL(url),
      base::BindOnce(
          [](PreviewPluginCallback callback,
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

void ShortcutsPageHandler::InstallPlugin(const std::string& url,
                                         const std::string& sha256,
                                         InstallPluginCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(profile_);
  if (!service) {
    std::move(callback).Run("Plugins aren't available here.");
    return;
  }
  service->InstallPlugin(GURL(url), sha256, std::move(callback));
}

void ShortcutsPageHandler::RemovePlugin(const std::string& id,
                                        RemovePluginCallback callback) {
  LiveFolderService* service = LiveFolderService::Get(profile_);
  if (!service) {
    std::move(callback).Run();
    return;
  }
  service->RemovePlugin(id, std::move(callback));
}

void ShortcutsPageHandler::OpenPluginsFolder() {
  if (LiveFolderService* service = LiveFolderService::Get(profile_)) {
    service->OpenPluginsFolder();
  }
}

void ShortcutsPageHandler::MakePluginWithClaude() {
  if (!profile_->GetPrefs()->GetBoolean(claude::prefs::kPlugins)) {
    return;
  }
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_contents_);
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  claude::ClaudePanel* panel =
      browser ? claude::ClaudePanel::From(browser) : nullptr;
  if (!panel) {
    return;
  }
  panel->Ask(
      "Help me make a Compass plugin: a live folder, or a preview that shows "
      "what matters on a site's tab when I hover it. Ask me what it should "
      "do, then look at the site and test it before saving.");
}

}  // namespace compass
