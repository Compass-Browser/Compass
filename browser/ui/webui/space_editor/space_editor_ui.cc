// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/space_editor/space_editor_ui.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/share_bubble/share_bubble.h"
#include "compass/browser/ui/webui/bubble/bubble_look_mojo.h"
#include "compass/browser/ui/webui/sidebar/space_mojo.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "compass/grit/compass_sidebar_resources.h"
#include "compass/grit/compass_sidebar_resources_map.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "net/base/url_util.h"
#include "ui/webui/webui_util.h"

namespace compass {

GURL SpaceEditorUrl(const std::string& space_id) {
  return GURL(base::StrCat({content::kChromeUIScheme, "://",
                            kCompassSpaceEditorHost, "/?id=", space_id}));
}

// The UI ----------------------------------------------------------------------

CompassSpaceEditorUIConfig::CompassSpaceEditorUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  kCompassSpaceEditorHost) {}

bool CompassSpaceEditorUIConfig::ShouldAutoResizeHost() {
  // The bubble grows and shrinks with the editor (the theme editor and the
  // emoji picker are taller than the menu).
  return true;
}

CompassSpaceEditorUI::CompassSpaceEditorUI(content::WebUI* web_ui,
                                           const GURL& url)
    : TopChromeWebUIController(web_ui) {
  net::GetValueForKeyInQuery(url, "id", &space_id_);
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kCompassSpaceEditorHost);
  webui::SetupWebUIDataSource(source, kCompassSidebarResources,
                              IDR_COMPASS_SIDEBAR_SPACE_EDITOR_HTML);
  content::URLDataSource::Add(profile, std::make_unique<ThemeSource>(profile));
}

CompassSpaceEditorUI::~CompassSpaceEditorUI() = default;

void CompassSpaceEditorUI::BindInterface(
    mojo::PendingReceiver<space_editor::mojom::PageHandlerFactory> receiver) {
  factory_receiver_.reset();
  factory_receiver_.Bind(std::move(receiver));
}

void CompassSpaceEditorUI::CreatePageHandler(
    mojo::PendingRemote<space_editor::mojom::Page> page,
    mojo::PendingReceiver<space_editor::mojom::PageHandler> receiver) {
  // Only in a window's bubble (ShowSpaceEditorBubble()): the window says
  // whose spaces these are.
  BrowserWindowInterface* browser =
      webui::GetBrowserWindowInterface(web_ui()->GetWebContents());
  if (!browser || !WindowFeatures::From(browser)) {
    return;
  }
  handler_ = std::make_unique<SpaceEditorPageHandler>(
      std::move(receiver), std::move(page), this, browser);
}

WEB_UI_CONTROLLER_TYPE_IMPL(CompassSpaceEditorUI)

// The handler -----------------------------------------------------------------

SpaceEditorPageHandler::SpaceEditorPageHandler(
    mojo::PendingReceiver<space_editor::mojom::PageHandler> receiver,
    mojo::PendingRemote<space_editor::mojom::Page> page,
    CompassSpaceEditorUI* controller,
    BrowserWindowInterface* browser)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      controller_(controller),
      browser_(browser) {
  // The same sources as the sidebar's switcher (SidebarPageHandler).
  list_observation_.Observe(
      &WindowFeatures::From(browser_)->service().space_list());
  if (SpaceDirectory::IsListable(browser_->GetProfile())) {
    if (SpaceDirectory* directory = SpaceDirectory::Get()) {
      uses_directory_ = true;
      directory_observation_.Observe(directory);
    }
  }
}

SpaceEditorPageHandler::~SpaceEditorPageHandler() = default;

void SpaceEditorPageHandler::GetSpace(GetSpaceCallback callback) {
  SpaceDirectory::Entry entry = FindAnySpace(controller_->space_id());
  if (!entry.space) {
    std::move(callback).Run(nullptr, {}, false);
    return;
  }
  size_t count = 0;
  std::vector<sidebar::mojom::ProfileOptionPtr> profiles;
  if (SpaceDirectory* directory = this->directory()) {
    count = directory->GetSpaces().size();
    profiles = GetProfileOptions();
  } else {
    count = WindowFeatures::From(browser_)->service().space_list().spaces().size();
  }
  std::move(callback).Run(ToMojoSpace(entry, browser_->GetProfile()),
                          std::move(profiles), count > 1);
}

void SpaceEditorPageHandler::RenameSpace(const std::string& id,
                                         const std::string& name) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::RenameSpace(*space, name);
  }
}

void SpaceEditorPageHandler::SetSpaceColor(
    const std::string& id,
    sidebar::mojom::SpaceColorPtr color) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceColor(*space, color);
  }
}

void SpaceEditorPageHandler::SetSpaceGradient(
    const std::string& id,
    std::vector<sidebar::mojom::SpaceColorPtr> gradient) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceGradient(*space, gradient);
  }
}

void SpaceEditorPageHandler::SetSpaceIntensity(const std::string& id,
                                               double intensity) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceIntensity(*space, intensity);
  }
}

void SpaceEditorPageHandler::SetSpaceGrain(const std::string& id,
                                           double grain) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceGrain(*space, grain);
  }
}

void SpaceEditorPageHandler::SetSpaceTranslucency(const std::string& id,
                                                  double translucency) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceTranslucency(*space, translucency);
  }
}

void SpaceEditorPageHandler::SetSpaceAppearance(
    const std::string& id,
    sidebar::mojom::SpaceAppearance appearance) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceAppearance(*space, appearance);
  }
}

void SpaceEditorPageHandler::SetSpaceIcon(const std::string& id,
                                          sidebar::mojom::SpaceIconPtr icon) {
  if (SpaceModel* space = FindEditableSpace(id)) {
    compass::SetSpaceIcon(*space, icon);
  }
}

void SpaceEditorPageHandler::SetArchiveAfter(const std::string& id,
                                             uint32_t hours) {
  if (SpaceModel* space = FindAnySpace(id).space) {
    SetSpaceArchiveAfter(*space, hours);
  }
}

void SpaceEditorPageHandler::SetSpaceProfile(
    const std::string& id,
    const std::optional<std::string>& profile_id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (!entry.space || !directory() || !g_browser_process->profile_manager()) {
    return;
  }
  std::optional<base::FilePath> path;
  if (profile_id) {
    path = ProfilePathFromId(*profile_id);
    if (!path) {
      return;
    }
  }
  AssignSpaceToProfile(entry.space->id(), std::move(path));
}

void SpaceEditorPageHandler::SetSpaceSynced(const std::string& id,
                                            bool synced) {
  if (SpaceModel* space = FindAnySpace(id).space) {
    space->SetSynced(synced);
  }
}

void SpaceEditorPageHandler::DeleteSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  if (entry.space) {
    compass::DeleteSpace(entry.profile, *entry.space);
  }
}

void SpaceEditorPageHandler::ShareSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  // Only the window's own profile's spaces, as the sidebar offered.
  if (!entry.space || entry.profile != browser_->GetProfile()) {
    return;
  }
  // The share bubble takes this one's place, pointing at the same button.
  // Copied first: closing the bubble destroys this handler.
  BrowserWindowInterface* browser = browser_;
  const gfx::Rect anchor = controller_->anchor();
  Close();
  ShowShareBubble(browser, "space", id, anchor);
}

void SpaceEditorPageHandler::GetLook(GetLookCallback callback) {
  std::move(callback).Run(
      GetBubbleLookFor(controller_->web_ui()->GetWebContents()));
}

void SpaceEditorPageHandler::ShowUI() {
  if (controller_->embedder()) {
    controller_->embedder()->ShowUI();
  }
}

void SpaceEditorPageHandler::Close() {
  if (controller_->embedder()) {
    controller_->embedder()->CloseUI();
  }
}

void SpaceEditorPageHandler::OnSpaceAdded(SpaceModel& space) {
  // The directory relays the list's changes too.
  if (!uses_directory_) {
    NotifyChanged();
  }
}

void SpaceEditorPageHandler::OnSpaceUpdated(const SpaceModel& space) {
  if (!uses_directory_) {
    NotifyChanged();
  }
}

void SpaceEditorPageHandler::OnSpaceRemoved() {
  if (!uses_directory_) {
    NotifyChanged();
  }
}

void SpaceEditorPageHandler::OnSpaceMoved(const SpaceModel& space) {
  if (!uses_directory_) {
    NotifyChanged();
  }
}

void SpaceEditorPageHandler::OnDirectoryChanged() {
  NotifyChanged();
}

SpaceDirectory::Entry SpaceEditorPageHandler::FindAnySpace(
    const std::string& id) {
  const base::Uuid uuid = base::Uuid::ParseLowercase(id);
  if (!uuid.is_valid()) {
    return SpaceDirectory::Entry();
  }
  if (SpaceModel* space =
          WindowFeatures::From(browser_)->service().space_list().FindSpace(
              uuid)) {
    return {browser_->GetProfile(), space};
  }
  SpaceDirectory* directory = this->directory();
  return directory ? directory->FindSpace(uuid) : SpaceDirectory::Entry();
}

SpaceModel* SpaceEditorPageHandler::FindEditableSpace(const std::string& id) {
  SpaceDirectory::Entry entry = FindAnySpace(id);
  return entry.space && !IsViewOnlySpace(entry.profile, *entry.space)
             ? entry.space.get()
             : nullptr;
}

SpaceDirectory* SpaceEditorPageHandler::directory() {
  // Gone with the profile manager, during shutdown.
  return uses_directory_ ? SpaceDirectory::GetIfExists() : nullptr;
}

void SpaceEditorPageHandler::NotifyChanged() {
  // A drag in the theme editor changes the space many times a second.
  if (change_pending_) {
    return;
  }
  change_pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SpaceEditorPageHandler::SendChanged,
                                weak_factory_.GetWeakPtr()));
}

void SpaceEditorPageHandler::SendChanged() {
  change_pending_ = false;
  page_->SpaceChanged();
}

}  // namespace compass
