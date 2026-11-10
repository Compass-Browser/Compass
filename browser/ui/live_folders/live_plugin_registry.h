// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_PLUGIN_REGISTRY_H_
#define COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_PLUGIN_REGISTRY_H_

#include <string>
#include <string_view>
#include <vector>

#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "compass/browser/live_folders/live_plugin_package.h"
#include "url/gurl.h"

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
  // A URL the sidebar can show its icon from: "/plugins/builtin/rss/icon.svg"
  // on its own origin for a built-in, a data: URL for an installed plugin, or
  // empty.
  std::string IconUrl() const {
    if (!icon_data_url.empty()) {
      return icon_data_url;
    }
    return manifest.icon.empty() || !builtin ? std::string()
                                             : "/" + base_path + manifest.icon;
  }

  // An installed plugin's icon, read when it was loaded.
  std::string icon_data_url;
  // An installed plugin's address and hash, which sync with its folders.
  GURL source;
  std::string sha256;
  // A local plugin: read from a folder the user put in Compass Plugins, and
  // removed by removing that folder.
  bool local = false;
};

// The plugins a profile can pick for a live folder: the built-ins, which
// ship in Compass's WebUI resources (resources/sidebar/BUILD.gn,
// :build_live_plugins_grdp). Installed plugins come with step 5 of
// docs/design/live-folders.md, "Order".
class LivePluginRegistry {
 public:
  // The browser's, shared by its profiles.
  static LivePluginRegistry& Get();

  LivePluginRegistry();
  LivePluginRegistry(const LivePluginRegistry&) = delete;
  LivePluginRegistry& operator=(const LivePluginRegistry&) = delete;
  ~LivePluginRegistry();

  const std::vector<LivePlugin>& plugins() const { return plugins_; }
  const LivePlugin* Find(std::string_view id) const;

  // Installed plugins (<user data>/Compass Plugins), served on
  // chrome-untrusted://live-folders/installed/<id>/<version>/, or for a
  // local plugin installed/<its folder>/. Adding one
  // replaces the plugin with its ID. Built-ins can't be replaced or removed.
  void AddInstalled(const LivePluginPackage& package);
  void RemoveInstalled(std::string_view id);
  // Replaces every installed and local plugin with |packages| (as read
  // again from <user data>/Compass Plugins).
  void ReplaceInstalled(const std::vector<LivePluginPackage>& packages);
  // Whether the installed plugins were read yet.
  bool installed_loaded() const { return installed_loaded_; }
  void set_installed_loaded() { installed_loaded_ = true; }

 private:
  std::vector<LivePlugin> plugins_;
  bool installed_loaded_ = false;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_LIVE_FOLDERS_LIVE_PLUGIN_REGISTRY_H_
