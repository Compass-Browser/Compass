// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_LIVE_FOLDERS_LIVE_PLUGIN_PACKAGE_H_
#define COMPASS_BROWSER_LIVE_FOLDERS_LIVE_PLUGIN_PACKAGE_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/types/expected.h"
#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
}

namespace compass {

// An installable plugin: its manifest and files, as fetched from where the
// user pointed (docs/design/live-folders.md, "Plugins"). Installed into
// <user data>/Compass Plugins/<id>/<version>/.
struct LivePluginPackage {
  LivePluginPackage();
  LivePluginPackage(const LivePluginPackage&);
  LivePluginPackage& operator=(const LivePluginPackage&);
  LivePluginPackage(LivePluginPackage&&);
  LivePluginPackage& operator=(LivePluginPackage&&);
  ~LivePluginPackage();

  LivePluginManifest manifest;
  // The files, as fetched.
  std::string manifest_json;
  std::string entry;
  // Empty if it has none.
  std::string icon;
  // The manifest's URL: where devices that sync a folder of this plugin
  // fetch it from.
  GURL source;
  // LivePluginHash() of the files.
  std::string sha256;
  // A local plugin's folder, directly in <user data>/Compass Plugins (its
  // files are read from there, and it has no |source|); empty for an
  // installed one.
  std::string local_dir;
};

// The plugin's hash, which syncs with its folders so another device installs
// the same files: lowercase hex SHA-256 of the manifest, the entry and the
// icon, each preceded by its length as 8 bytes, little-endian.
std::string LivePluginHash(const std::string& manifest_json,
                           const std::string& entry,
                           const std::string& icon);

// Checks fetched files and makes a package: the manifest must be valid and
// not claim a built-in's "compass." ID.
base::expected<LivePluginPackage, std::string> MakeLivePluginPackage(
    const GURL& source,
    std::string manifest_json,
    std::string entry,
    std::string icon);

using LivePluginPackageCallback =
    base::OnceCallback<void(base::expected<LivePluginPackage, std::string>)>;

// Fetches the plugin whose manifest is at |manifest_url| (https, or http
// for localhost), with its entry and icon from beside it.
void FetchLivePluginPackage(
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    const GURL& manifest_url,
    LivePluginPackageCallback callback);

// Where installed plugins live: <user data>/Compass Plugins.
base::FilePath LivePluginsDir(const base::FilePath& user_data_dir);

// Blocking: writes |package| into |plugins_dir|/<id>/<version>/, replacing
// other versions of it.
bool WriteLivePluginPackage(const base::FilePath& plugins_dir,
                            const LivePluginPackage& package);

// Blocking: the packages installed in |plugins_dir|, then its local plugins:
// folders right inside it with a manifest.json, for trying out plugins
// without hosting them. Ones that don't read or validate are skipped.
std::vector<LivePluginPackage> ReadLivePluginPackages(
    const base::FilePath& plugins_dir);

// Blocking: removes installed plugin |id| (never a local plugin's folder).
bool DeleteLivePluginPackage(const base::FilePath& plugins_dir,
                             const std::string& id);

// The file |relative| ("<id>/<version>/<name>", or "<folder>/<name>" for a
// local plugin) under |plugins_dir|, or an empty path if it reaches anywhere
// else.
base::FilePath LivePluginFilePath(const base::FilePath& plugins_dir,
                                  std::string_view relative);

}  // namespace compass

#endif  // COMPASS_BROWSER_LIVE_FOLDERS_LIVE_PLUGIN_PACKAGE_H_
