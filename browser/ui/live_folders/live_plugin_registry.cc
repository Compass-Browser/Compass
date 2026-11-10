// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/live_folders/live_plugin_registry.h"

#include <algorithm>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "ui/base/resource/resource_bundle.h"

namespace compass {

namespace {

constexpr std::string_view kBuiltinPrefix = "plugins/builtin/";
constexpr std::string_view kManifestName = "manifest.json";

}  // namespace

// static
LivePluginRegistry& LivePluginRegistry::Get() {
  static base::NoDestructor<LivePluginRegistry> registry;
  return *registry;
}

LivePluginRegistry::LivePluginRegistry() {
  for (const webui::ResourcePath& resource : kCompassSidebarResources) {
    std::string_view path = resource.path;
    if (!path.starts_with(kBuiltinPrefix) || !path.ends_with(kManifestName)) {
      continue;
    }
    std::optional<base::Value> raw = base::JSONReader::Read(
        ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
            resource.id),
        base::JSON_PARSE_RFC);
    if (!raw) {
      LOG(ERROR) << "Unreadable built-in plugin manifest " << path;
      continue;
    }
    auto manifest = ParseLivePluginManifest(*raw);
    if (!manifest.has_value()) {
      LOG(ERROR) << "Invalid built-in plugin manifest " << path << ": "
                 << manifest.error().front();
      continue;
    }
    LivePlugin plugin;
    plugin.manifest = std::move(*manifest);
    plugin.base_path =
        std::string(path.substr(0, path.size() - kManifestName.size()));
    plugin.builtin = true;
    plugins_.push_back(std::move(plugin));
  }
  std::ranges::sort(plugins_, {}, [](const LivePlugin& plugin) {
    return plugin.manifest.name;
  });
}

LivePluginRegistry::~LivePluginRegistry() = default;

void LivePluginRegistry::AddInstalled(const LivePluginPackage& package) {
  const LivePlugin* existing = Find(package.manifest.id);
  if (existing && existing->builtin) {
    return;
  }
  RemoveInstalled(package.manifest.id);
  LivePlugin plugin;
  plugin.manifest = package.manifest;
  plugin.base_path = "installed/" + package.manifest.id + "/" +
                     package.manifest.version + "/";
  if (!package.icon.empty()) {
    plugin.icon_data_url =
        std::string(package.manifest.icon.ends_with(".png")
                        ? "data:image/png;base64,"
                        : "data:image/svg+xml;base64,") +
        base::Base64Encode(package.icon);
  }
  plugin.source = package.source;
  plugin.sha256 = package.sha256;
  plugins_.push_back(std::move(plugin));
  std::ranges::sort(plugins_, {}, [](const LivePlugin& p) {
    return p.manifest.name;
  });
}

void LivePluginRegistry::RemoveInstalled(std::string_view id) {
  std::erase_if(plugins_, [id](const LivePlugin& plugin) {
    return !plugin.builtin && plugin.manifest.id == id;
  });
}

const LivePlugin* LivePluginRegistry::Find(std::string_view id) const {
  auto it = std::ranges::find(plugins_, id, [](const LivePlugin& plugin) {
    return std::string_view(plugin.manifest.id);
  });
  return it == plugins_.end() ? nullptr : &*it;
}

}  // namespace compass
