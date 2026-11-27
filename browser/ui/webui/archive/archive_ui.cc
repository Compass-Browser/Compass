// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/archive/archive_ui.h"

#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/ui/webui/bubble/bubble_look_mojo.h"
#include "compass/browser/ui/webui/localized_strings/localized_strings.h"
#include "compass/browser/ui/webui/sidebar/compass_favicon_source.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/webui/webui_util.h"

namespace compass {

GURL ArchiveUrl() {
  return GURL(
      base::StrCat({content::kChromeUIScheme, "://", kCompassArchiveHost, "/"}));
}

// The UI ----------------------------------------------------------------------

CompassArchiveUIConfig::CompassArchiveUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  kCompassArchiveHost) {}

bool CompassArchiveUIConfig::ShouldAutoResizeHost() {
  // The bubble grows and shrinks with the list.
  return true;
}

CompassArchiveUI::CompassArchiveUI(content::WebUI* web_ui, const GURL& url)
    : TopChromeWebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kCompassArchiveHost);
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_ARCHIVE_HTML);
  AddSidebarPakLocalizedStrings(source);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
  // The entries' icons (favicon.ts).
  content::URLDataSource::Add(profile,
                              std::make_unique<CompassFaviconSource>(profile));
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ImgSrc,
      "img-src chrome://resources chrome://theme chrome://compass-favicon "
      "data: 'self';");
}

CompassArchiveUI::~CompassArchiveUI() = default;

void CompassArchiveUI::BindInterface(
    mojo::PendingReceiver<archive::mojom::PageHandlerFactory> receiver) {
  factory_receiver_.reset();
  factory_receiver_.Bind(std::move(receiver));
}

void CompassArchiveUI::CreatePageHandler(
    mojo::PendingRemote<archive::mojom::Page> page,
    mojo::PendingReceiver<archive::mojom::PageHandler> receiver) {
  // Only in a window's bubble (ShowArchiveBubble()): the window says whose
  // archive this is.
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  WindowFeatures* features = browser ? WindowFeatures::From(browser) : nullptr;
  if (!features || features->swapped_away()) {
    return;
  }
  handler_ = std::make_unique<ArchivePageHandler>(
      std::move(receiver), std::move(page), this, browser);
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassArchiveUI)

// The handler -----------------------------------------------------------------

ArchivePageHandler::ArchivePageHandler(
    mojo::PendingReceiver<archive::mojom::PageHandler> receiver,
    mojo::PendingRemote<archive::mojom::Page> page,
    CompassArchiveUI* controller,
    BrowserWindowInterface* browser)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      controller_(controller),
      browser_(browser),
      features_(WindowFeatures::From(browser)) {
  features_->AddObserver(this);
  observed_space_ = &features_->space_model();
  observed_space_->AddObserver(this);
}

ArchivePageHandler::~ArchivePageHandler() {
  StopObservingSpace();
  features_->RemoveObserver(this);
}

void ArchivePageHandler::GetArchive(GetArchiveCallback callback) {
  std::vector<sidebar::mojom::ArchivedEntryPtr> entries;
  if (!observed_space_) {
    std::move(callback).Run(std::move(entries), 0);
    return;
  }
  for (const ArchivedEntry& entry : observed_space_->archive()) {
    auto mojo_entry = sidebar::mojom::ArchivedEntry::New();
    mojo_entry->id = entry.id.AsLowercaseString();
    mojo_entry->archived_at = entry.archived_at.InMillisecondsFSinceUnixEpoch();
    for (const ArchivedEntry::Tab& tab : entry.tabs) {
      mojo_entry->tabs.push_back(sidebar::mojom::ArchivedTab::New(
          base::UTF16ToUTF8(tab.title), tab.url));
    }
    entries.push_back(std::move(mojo_entry));
  }
  std::move(callback).Run(
      std::move(entries),
      static_cast<uint32_t>(
          observed_space_->archive_after().IntDiv(GetArchiveHour())));
}

void ArchivePageHandler::RestoreArchived(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (!uuid.is_valid() || !observed_space_ ||
      !features_->has_tab_strip_bridge()) {
    return;
  }
  SpaceModel& space = *observed_space_;
  SidebarNode* entry =
      space.Unarchive(uuid, space.today_root()->children().size());
  if (!entry) {
    return;
  }
  // A split opens its first pane.
  SidebarNode* tab = entry->type() == SidebarNode::Type::kSplit
                         ? entry->children()[0].get()
                         : entry;
  if (tabs::TabInterface* live = features_->tab_strip_bridge().OpenNode(tab)) {
    TabStripModel* tab_strip = browser_->GetTabStripModel();
    tab_strip->ActivateTabAt(tab_strip->GetIndexOfTab(live));
  }
  // Back to the tab it brought back.
  Close();
}

void ArchivePageHandler::RemoveArchived(const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (uuid.is_valid() && observed_space_) {
    observed_space_->RemoveArchived(uuid);
  }
}

void ArchivePageHandler::ClearArchive() {
  if (observed_space_) {
    observed_space_->ClearArchive();
  }
}

void ArchivePageHandler::GetLook(GetLookCallback callback) {
  std::move(callback).Run(
      GetBubbleLookFor(controller_->web_ui()->GetWebContents()));
}

void ArchivePageHandler::ShowUI() {
  if (controller_->embedder()) {
    controller_->embedder()->ShowUI();
  }
}

void ArchivePageHandler::Close() {
  if (controller_->embedder()) {
    controller_->embedder()->CloseUI();
  }
}

void ArchivePageHandler::OnArchiveChanged(const SpaceModel& space) {
  page_->ArchiveChanged();
}

void ArchivePageHandler::OnSpaceSwitched() {
  StopObservingSpace();
  observed_space_ = &features_->space_model();
  observed_space_->AddObserver(this);
  page_->ArchiveChanged();
}

void ArchivePageHandler::OnSwappedAway() {
  // The window empties and closes; its space can go first.
  StopObservingSpace();
  Close();
}

void ArchivePageHandler::StopObservingSpace() {
  if (observed_space_) {
    observed_space_->RemoveObserver(this);
    observed_space_ = nullptr;
  }
}

}  // namespace compass
