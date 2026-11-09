// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UPDATES_UPDATE_MANIFEST_H_
#define COMPASS_BROWSER_UPDATES_UPDATE_MANIFEST_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "url/gurl.h"

// The channel manifests on the update server (stable.json, beta.json,
// canary.json; written by `compass release`, tools/compass_tool/release.py),
// and the checks an update must pass before it's installed. The Windows
// updater uses them; macOS has Sparkle (docs/design/updates.md).
namespace compass::updates {

// The update server's root: the feeds, and installers released before GitHub.
inline constexpr char kUpdateServer[] = "https://updates.compassbrowser.org";
// Where newer installers are: assets of Compass's GitHub releases. GitHub
// redirects the download to its storage host.
inline constexpr char kGitHubReleaseDownloads[] =
    "https://github.com/Compass-Browser/Compass/releases/download/";

// The manifest |channel| ("stable", "beta" or "canary") reads.
GURL ChannelManifestUrl(std::string_view channel);

// One platform's entry ("mac", "win") of a channel manifest.
struct ManifestEntry {
  std::string version;   // Compass's, e.g. "20261103.0215".
  std::string chromium;  // Upstream's, e.g. "156.0.8078.48".
  GURL url;              // The installer.
  int64_t size = 0;
  std::string sha256;        // Lowercase hex.
  std::string ed_signature;  // Base64 Ed25519 signature of the file.
};

// |platform|'s entry in a manifest, or nullopt if the JSON doesn't have a
// valid one. Only https URLs on the update server count.
std::optional<ManifestEntry> ParseChannelManifest(std::string_view json,
                                                  std::string_view platform);

// Whether |candidate| is a newer Compass version than |running| (both
// numbers and dots, e.g. "20261103.0215"). An unparsable one is never newer.
bool IsNewerVersion(std::string_view candidate, std::string_view running);

// Whether |data| matches the entry: its size, SHA-256, and Ed25519 signature
// under |public_key_base64| (the release key, compass_sparkle_public_key).
bool VerifyDownload(base::span<const uint8_t> data,
                    const ManifestEntry& entry,
                    std::string_view public_key_base64);

}  // namespace compass::updates

#endif  // COMPASS_BROWSER_UPDATES_UPDATE_MANIFEST_H_
