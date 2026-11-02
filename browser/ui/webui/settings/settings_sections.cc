// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/settings/settings_sections.h"

#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/ui/webui/arc_import/compass_arc_import_ui.h"
#include "compass/browser/ui/webui/claude/claude_settings_handler.h"
#include "compass/browser/ui/webui/settings/compass_settings_ui.h"
#include "compass/browser/ui/webui/sync/compass_sync_ui.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/web_ui_data_source.h"

namespace compass {

namespace {

class SettingsSectionsImpl : public SettingsSections {
 public:
  explicit SettingsSectionsImpl(content::WebUI* web_ui) : web_ui_(web_ui) {}
  ~SettingsSectionsImpl() override = default;

  void Bind(mojo::PendingReceiver<sidebar::mojom::SettingsHandler> receiver)
      override {
    shortcuts_.reset();
    shortcuts_ = std::make_unique<ShortcutsPageHandler>(std::move(receiver));
  }

  void Bind(mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver)
      override {
    arc_import_.reset();
    arc_import_ =
        std::make_unique<ArcImportPageHandler>(web_ui_, std::move(receiver));
  }

  void Bind(mojo::PendingReceiver<sync::mojom::PageHandlerFactory> receiver)
      override {
    sync_.reset();
    sync_ =
        std::make_unique<SyncPageHandlerFactory>(web_ui_, std::move(receiver));
  }

  void Bind(mojo::PendingReceiver<claude::mojom::SettingsHandler> receiver)
      override {
    claude_.reset();
    claude_ = std::make_unique<claude::ClaudeSettingsHandler>(
        Profile::FromWebUI(web_ui_), std::move(receiver));
  }

 private:
  const raw_ptr<content::WebUI> web_ui_;
  std::unique_ptr<ShortcutsPageHandler> shortcuts_;
  std::unique_ptr<ArcImportPageHandler> arc_import_;
  std::unique_ptr<SyncPageHandlerFactory> sync_;
  std::unique_ptr<claude::ClaudeSettingsHandler> claude_;
};

}  // namespace

// static
std::unique_ptr<SettingsSections> SettingsSections::Create(
    content::WebUI* web_ui,
    content::WebUIDataSource* source) {
  // The whole Compass pak: the sections import the mojo bindings next to
  // them. The sidebar's own pages are there too, unused.
  for (const webui::ResourcePath& resource : kCompassSidebarResources) {
    source->AddResourcePath(base::StrCat({"compass/", resource.path}),
                            resource.id);
  }
  return std::make_unique<SettingsSectionsImpl>(web_ui);
}

}  // namespace compass
