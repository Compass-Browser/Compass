// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SIDEBAR_LIVE_PLUGIN_MOJO_H_
#define COMPASS_BROWSER_UI_WEBUI_SIDEBAR_LIVE_PLUGIN_MOJO_H_

#include <vector>

#include "compass/browser/live_folders/live_plugin_manifest.h"
#include "compass/browser/live_folders/live_plugin_package.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"

namespace compass {

// A plugin's settings form fields (a live folder's, or a tab preview's), for
// the pages that build the form (the live folder editor, Settings › Plugins).
std::vector<sidebar::mojom::LiveConfigFieldPtr> ToMojoConfigFields(
    const std::vector<LivePluginManifest::ConfigField>& fields);

// The sites a tab preview reads pages on, for people: "calendar.google.com",
// "*.example.com".
std::vector<std::string> PreviewSites(
    const LivePluginManifest::Preview& preview);

// A fetched plugin, for the user to review before installing: what it
// reaches, asks for and reads.
sidebar::mojom::LivePluginPreviewPtr ToMojoPluginPreview(
    const LivePluginPackage& package);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SIDEBAR_LIVE_PLUGIN_MOJO_H_
