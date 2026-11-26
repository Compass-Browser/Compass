// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/arc_import/compass_arc_import_ui.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "compass/browser/ui/webui/localized_strings/localized_strings.h"
#include "components/tabs/public/tab_interface.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/arc_import/arc_importer.h"
#include "compass/browser/ui/browser_import/app_icon.h"
#include "compass/browser/ui/browser_import/browser_importer.h"
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
  AddSidebarPakLocalizedStrings(source);
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
        space_entry->tab_count =
            CountTabs(space.pinned) + CountTabs(space.today);
        space_entry->archived_count =
            base::checked_cast<int32_t>(space.archive.size());
        space_entry->imported =
            entry->imported && imported->second.spaces.contains(space.id);
        tabs += space_entry->tab_count;
        entry->spaces.push_back(std::move(space_entry));
      }
      entry->tab_count = tabs;
      state->profiles.push_back(std::move(entry));
    }
  }
  if (!state->found) {
    std::move(callback).Run(std::move(state));
    return;
  }
  // Arc's own icon, for its row (browser_import/app_icon.h).
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&browser_import::ArcAppIcon),
      base::BindOnce(
          [](GetStateCallback callback,
             sidebar::mojom::ArcImportStatePtr state, std::string icon) {
            state->icon_data_url = std::move(icon);
            std::move(callback).Run(std::move(state));
          },
          std::move(callback), std::move(state)));
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
    std::move(callback).Run({}, {}, profiles, 0, 0, 0);
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
                          result.failed, result.undecryptable, result.easels,
                          result.easels_missing);
}

void ArcImportPageHandler::ImportEasels(ImportEaselsCallback callback) {
  // Its cache is being written while it runs.
  if (arc::IsArcRunning()) {
    std::move(callback).Run(true, 0, 0);
    return;
  }
  arc::ImportArcEaselsAgain(base::BindOnce(
      &ArcImportPageHandler::OnEaselsImported, weak_factory_.GetWeakPtr(),
      std::move(callback)));
}

void ArcImportPageHandler::OnEaselsImported(ImportEaselsCallback callback,
                                            arc::ArcImportResult result) {
  std::move(callback).Run(false, result.easels, result.easels_missing);
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

void ArcImportPageHandler::GetBrowsers(GetBrowsersCallback callback) {
  browser_import::GetBrowserImportOverview(
      Profile::FromWebUI(web_ui_),
      base::BindOnce(&ArcImportPageHandler::OnBrowsers,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ArcImportPageHandler::OnBrowsers(
    GetBrowsersCallback callback,
    std::vector<browser_import::ProfileOverview> profiles) {
  std::vector<sidebar::mojom::BrowserImportProfilePtr> entries;
  for (const browser_import::ProfileOverview& profile : profiles) {
    auto entry = sidebar::mojom::BrowserImportProfile::New();
    entry->id = profile.source.id;
    entry->browser =
        base::UTF16ToUTF8(browser_import::BrowserName(profile.source.browser));
    entry->name = base::UTF16ToUTF8(profile.source.name);
    for (const std::u16string& space : profile.spaces) {
      entry->spaces.push_back(base::UTF16ToUTF8(space));
    }
    entry->bookmark_count = profile.bookmarks;
    entry->tab_count = profile.tabs;
    entry->running = profile.running;
    entry->imported = profile.imported;
    entry->history = profile.history;
    entry->passwords = profile.passwords;
    entry->note = profile.note;
    entry->as_profile = profile.as_profile;
    entry->as_profile_brings = profile.as_profile_brings;
    entry->imported_as_profile = base::UTF16ToUTF8(profile.imported_as_profile);
    entry->newer_version = profile.newer_version;
    entry->icon_data_url = profile.icon;
    entries.push_back(std::move(entry));
  }
  std::move(callback).Run(std::move(entries));
}

void ArcImportPageHandler::ImportBrowsers(
    std::vector<sidebar::mojom::BrowserImportChoicePtr> choices,
    ImportBrowsersCallback callback) {
  std::vector<browser_import::BrowserImportChoice> chosen;
  for (const auto& choice : choices) {
    chosen.push_back({choice->id, choice->bookmarks, choice->tabs,
                      choice->history, choice->passwords, choice->new_profile});
  }
  browser_import::ImportFromBrowsers(
      Profile::FromWebUI(web_ui_), std::move(chosen),
      base::BindOnce(&ArcImportPageHandler::OnBrowsersImported,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ArcImportPageHandler::OnBrowsersImported(
    ImportBrowsersCallback callback,
    browser_import::BrowserImportResult result) {
  if (result.first_space.is_valid() && result.first_space_profile) {
    imported_profile_ = result.first_space_profile;
    imported_space_ = result.first_space;
  }
  std::vector<sidebar::mojom::BrowserImportSummaryPtr> summaries;
  for (const browser_import::BrowserImportSummary& summary :
       result.summaries) {
    auto entry = sidebar::mojom::BrowserImportSummary::New();
    entry->name = base::UTF16ToUTF8(summary.name);
    for (const std::u16string& space : summary.spaces) {
      entry->spaces.push_back(base::UTF16ToUTF8(space));
    }
    entry->pinned = summary.pinned;
    entry->today = summary.today;
    entry->favorites = summary.favorites;
    entry->history = summary.history;
    entry->passwords = summary.passwords;
    entry->passwords_skipped = summary.passwords_skipped;
    entry->profile = base::UTF16ToUTF8(summary.profile);
    entry->brought = summary.brought;
    entry->cookies_dropped = summary.cookies_dropped;
    entry->problems = summary.problems;
    summaries.push_back(std::move(entry));
  }
  std::move(callback).Run(std::move(summaries));
}

}  // namespace compass
