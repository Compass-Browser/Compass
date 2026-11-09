// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_PLUGIN_REGISTRY_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_PLUGIN_REGISTRY_H_

#include <string>
#include <string_view>
#include <vector>

#include "compass/browser/live_folders/live_plugin_manifest.h"

namespace compass {

// A plugin the browser can run.
struct LivePlugin {
  LivePluginManifest manifest;
  // Where its files are served, on chrome-untrusted://live-folders/ and
  // chrome://compass-sidebar/ alike: "plugins/builtin/rss/".
  std::string base_path;
  bool builtin = false;

  // "/plugins/builtin/rss/main.js".
  std::string EntryPath() const { return "/" + base_path + manifest.entry; }
  // "plugins/builtin/rss/icon.svg", or empty.
  std::string IconPath() const {
    return manifest.icon.empty() ? std::string() : base_path + manifest.icon;
  }
};

// The plugins a profile can pick for a live folder: the built-ins, which
// ship in Compass's WebUI resources (resources/sidebar/BUILD.gn,
// :build_live_plugins_grdp). Installed plugins come with step 5 of
// docs/design/live-folders.md, "Order".
class LivePluginRegistry {
 public:
  // The browser's, shared by its profiles.
  static const LivePluginRegistry& Get();

  LivePluginRegistry();
  LivePluginRegistry(const LivePluginRegistry&) = delete;
  LivePluginRegistry& operator=(const LivePluginRegistry&) = delete;
  ~LivePluginRegistry();

  const std::vector<LivePlugin>& plugins() const { return plugins_; }
  const LivePlugin* Find(std::string_view id) const;

 private:
  std::vector<LivePlugin> plugins_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_PLUGIN_REGISTRY_H_
