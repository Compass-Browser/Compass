// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/sidebar/live_plugin_mojo.h"

#include <algorithm>
#include <string>

#include "base/json/json_writer.h"

namespace compass {

std::vector<sidebar::mojom::LiveConfigFieldPtr> ToMojoConfigFields(
    const std::vector<LivePluginManifest::ConfigField>& fields) {
  using Type = LivePluginManifest::ConfigField::Type;
  std::vector<sidebar::mojom::LiveConfigFieldPtr> result;
  for (const auto& field : fields) {
    auto f = sidebar::mojom::LiveConfigField::New();
    f->key = field.key;
    switch (field.type) {
      case Type::kString:
        f->type = "string";
        break;
      case Type::kUrl:
        f->type = "url";
        break;
      case Type::kNumber:
        f->type = "number";
        break;
      case Type::kBool:
        f->type = "bool";
        break;
      case Type::kSelect:
        f->type = "select";
        break;
    }
    f->label = field.label;
    f->placeholder = field.placeholder;
    f->optional = field.optional;
    if (field.default_value) {
      f->default_json = base::WriteJson(*field.default_value).value_or("");
    }
    f->min = field.min;
    f->max = field.max;
    for (const auto& option : field.options) {
      f->options.push_back(
          sidebar::mojom::LiveSelectOption::New(option.value, option.label));
    }
    result.push_back(std::move(f));
  }
  return result;
}

std::vector<std::string> PreviewSites(
    const LivePluginManifest::Preview& preview) {
  std::vector<std::string> sites;
  for (const std::string& match : preview.matches) {
    const size_t start = match.find("://");
    if (start == std::string::npos) {
      continue;
    }
    const size_t end = match.find('/', start + 3);
    std::string host = match.substr(start + 3, end - start - 3);
    if (!std::ranges::contains(sites, host)) {
      sites.push_back(std::move(host));
    }
  }
  return sites;
}

sidebar::mojom::LivePluginPreviewPtr ToMojoPluginPreview(
    const LivePluginPackage& package) {
  const LivePluginManifest& manifest = package.manifest;
  auto preview = sidebar::mojom::LivePluginPreview::New();
  preview->id = manifest.id;
  preview->name = manifest.name;
  preview->description = manifest.description;
  preview->version = manifest.version;
  preview->hosts = manifest.hosts;
  preview->cookie_hosts = manifest.cookies;
  for (const auto& secret : manifest.secrets) {
    preview->secrets.push_back(secret.label);
  }
  if (manifest.preview) {
    preview->preview_sites = PreviewSites(*manifest.preview);
    if (manifest.preview->session) {
      preview->session_site = manifest.preview->session->site;
      preview->related_hosts = manifest.preview->session->related;
    }
  }
  preview->source = package.source.spec();
  preview->sha256 = package.sha256;
  return preview;
}

}  // namespace compass
