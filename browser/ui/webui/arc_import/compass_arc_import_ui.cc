// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/arc_import/compass_arc_import_ui.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/arc_import/arc_importer.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "ui/webui/webui_util.h"

namespace compass {

namespace {

// How long QuitArc() waits for Arc to go.
constexpr base::TimeDelta kQuitCheckInterval = base::Milliseconds(250);
constexpr int kQuitChecks = 40;

int CountTabs(const std::vector<arc::Item>& items) {
  int count = 0;
  for (const arc::Item& item : items) {
    count += item.kind == arc::Item::Kind::kTab ? 1 : CountTabs(item.children);
  }
  return count;
}

}  // namespace

CompassArcImportUIConfig::CompassArcImportUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kArcImportHost) {}

CompassArcImportUI::CompassArcImportUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kArcImportHost);
  // Shares the sidebar's resource pak; only the default page differs.
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_ARC_IMPORT_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassArcImportUI::~CompassArcImportUI() = default;

void CompassArcImportUI::BindInterface(
    mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver) {
  handler_.reset();
  handler_ =
      std::make_unique<ArcImportPageHandler>(web_ui(), std::move(receiver));
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassArcImportUI)

ArcImportPageHandler::ArcImportPageHandler(
    content::WebUI* web_ui,
    mojo::PendingReceiver<sidebar::mojom::ArcImportHandler> receiver)
    : web_ui_(web_ui), receiver_(this, std::move(receiver)) {}

ArcImportPageHandler::~ArcImportPageHandler() = default;

void ArcImportPageHandler::GetState(GetStateCallback callback) {
  arc::GetArcOverview(base::BindOnce(&ArcImportPageHandler::OnOverview,
                                     weak_factory_.GetWeakPtr(),
                                     std::move(callback)));
}

void ArcImportPageHandler::OnOverview(GetStateCallback callback,
                                    arc::ArcOverview overview) {
  auto state = sidebar::mojom::ArcImportState::New();
  state->found = overview.sidebar.has_value();
  state->arc_running = arc::IsArcRunning();
  if (overview.sidebar) {
    for (const arc::Profile& profile : overview.sidebar->profiles) {
      const auto imported = overview.imported.find(profile.dir);
      auto entry = sidebar::mojom::ArcImportProfile::New();
      entry->dir = profile.dir;
      entry->name = base::UTF16ToUTF8(profile.name);
      entry->imported = imported != overview.imported.end();
      int tabs = CountTabs(profile.favorites);
      for (const arc::Space& space : overview.sidebar->spaces) {
        if (space.profile_dir != profile.dir) {
          continue;
        }
        auto space_entry = sidebar::mojom::ArcImportSpace::New();
        space_entry->id = space.id;
        space_entry->name = base::UTF16ToUTF8(space.name);
        space_entry->tab_count = CountTabs(space.pinned) + CountTabs(space.today);
        space_entry->imported =
            entry->imported && imported->second.spaces.contains(space.id);
        tabs += space_entry->tab_count;
        entry->spaces.push_back(std::move(space_entry));
      }
      entry->tab_count = tabs;
      state->profiles.push_back(std::move(entry));
    }
  }
  std::move(callback).Run(std::move(state));
}

void ArcImportPageHandler::QuitArc(QuitArcCallback callback) {
  if (!arc::IsArcRunning()) {
    std::move(callback).Run(true);
    return;
  }
  arc::QuitArc();
  CheckArcQuit(std::move(callback), kQuitChecks);
}

void ArcImportPageHandler::CheckArcQuit(QuitArcCallback callback,
                                      int attempts_left) {
  if (!arc::IsArcRunning() || attempts_left <= 0) {
    std::move(callback).Run(!arc::IsArcRunning());
    return;
  }
  quit_timer_.Start(
      FROM_HERE, kQuitCheckInterval,
      base::BindOnce(&ArcImportPageHandler::CheckArcQuit,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     attempts_left - 1));
}

void ArcImportPageHandler::Import(const std::vector<std::string>& profiles,
                                  const std::vector<std::string>& spaces,
                                  ImportCallback callback) {
  // Its databases are in use while it runs.
  if (arc::IsArcRunning() || (profiles.empty() && spaces.empty())) {
    std::move(callback).Run({}, {}, profiles);
    return;
  }
  arc::ArcImportChoice choice;
  choice.profiles = profiles;
  choice.spaces.emplace(spaces.begin(), spaces.end());
  arc::ImportFromArc(std::move(choice),
                     base::BindOnce(&ArcImportPageHandler::OnImported,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(callback)));
}

void ArcImportPageHandler::OnImported(ImportCallback callback,
                                      arc::ArcImportResult result) {
  ProfileAttributesStorage& storage =
      g_browser_process->profile_manager()->GetProfileAttributesStorage();
  auto names = [&](const std::vector<raw_ptr<Profile>>& profiles) {
    std::vector<std::string> names;
    for (const raw_ptr<Profile>& profile : profiles) {
      ProfileAttributesEntry* entry =
          storage.GetProfileAttributesWithPath(profile->GetPath());
      names.push_back(entry ? base::UTF16ToUTF8(entry->GetLocalProfileName())
                            : profile->GetBaseName().AsUTF8Unsafe());
    }
    return names;
  };
  imported_profile_.reset();
  imported_space_ = base::Uuid();
  if (result.first_space.is_valid()) {
    // The space belongs to the first profile that got spaces.
    for (const auto* profiles : {&result.profiles, &result.updated}) {
      for (const raw_ptr<Profile>& profile : *profiles) {
        if (!imported_profile_ &&
            SpaceService::Get(profile)->space_list().FindSpace(
                result.first_space)) {
          imported_profile_ = profile->GetWeakPtr();
          imported_space_ = result.first_space;
        }
      }
    }
  }
  std::move(callback).Run(names(result.profiles), names(result.updated),
                          result.failed);
}

void ArcImportPageHandler::OpenImported() {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui_->GetWebContents());
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser || !imported_profile_) {
    return;
  }
  SpaceModel* space = SpaceService::Get(imported_profile_.get())
                          ->space_list()
                          .FindSpace(imported_space_);
  if (space) {
    SwitchWindowToSpace(browser, imported_profile_.get(), *space);
  }
}

}  // namespace compass
