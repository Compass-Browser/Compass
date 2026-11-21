// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings window's General and Profiles panes (phase 2,
// docs/design/settings-window.md): what upstream's settings handlers
// (DefaultBrowserHandler, SearchEnginesHandler, DownloadsHandler,
// FontHandler, SystemHandler, ProfileInfoHandler, ManageProfileHandler) do
// for chrome://settings, over the window's Mojo interface.

#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_avatar_icon_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "chrome/browser/ui/webui/settings/settings_utils.h"
#include "chrome/common/pref_names.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/choice_made_location.h"
#include "components/search_engines/template_url.h"
#include "content/public/browser/font_list_async.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/webui/web_ui_util.h"
#include "ui/gfx/image/image.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace compass {

namespace {

bool DefaultBrowserDisabledByPolicy() {
  const PrefService::Preference* pref =
      g_browser_process->local_state()->FindPreference(
          ::prefs::kDefaultBrowserSettingEnabled);
  return pref && pref->IsManaged() && !pref->GetValue()->GetBool();
}

// The picture's size in the page, at 2x.
constexpr int kProfileIconSize = 80;

}  // namespace

// Default browser -------------------------------------------------------------

void SettingsWindowHandler::OnDefaultBrowserState(
    base::OnceCallback<void(sidebar::mojom::DefaultBrowserStatePtr)> callback,
    shell_integration::DefaultWebClientState state) {
  std::move(callback).Run(sidebar::mojom::DefaultBrowserState::New(
      state == shell_integration::IS_DEFAULT,
      shell_integration::CanSetAsDefaultBrowser(),
      state == shell_integration::UNKNOWN_DEFAULT,
      DefaultBrowserDisabledByPolicy()));
}

void SettingsWindowHandler::GetDefaultBrowser(
    GetDefaultBrowserCallback callback) {
  base::MakeRefCounted<shell_integration::DefaultBrowserWorker>()
      ->StartCheckIsDefault(
          base::BindOnce(&SettingsWindowHandler::OnDefaultBrowserState,
                         weak_factory_.GetWeakPtr(), std::move(callback)));
}

void SettingsWindowHandler::SetAsDefaultBrowser() {
  if (DefaultBrowserDisabledByPolicy()) {
    return;
  }
  // Tells the page the state once the system has answered.
  base::MakeRefCounted<shell_integration::DefaultBrowserWorker>()
      ->StartSetAsDefault(base::BindOnce(
          &SettingsWindowHandler::OnDefaultBrowserState,
          weak_factory_.GetWeakPtr(),
          base::BindOnce(
              [](base::WeakPtr<SettingsWindowHandler> handler,
                 sidebar::mojom::DefaultBrowserStatePtr state) {
                if (handler) {
                  handler->page_->DefaultBrowserChanged(std::move(state));
                }
              },
              weak_factory_.GetWeakPtr())));
}

// Search engines
// ---------------------------------------------------------------

sidebar::mojom::SearchEnginesPtr
SettingsWindowHandler::DescribeSearchEngines() {
  auto result = sidebar::mojom::SearchEngines::New();
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(profile_);
  if (!service || !service->loaded()) {
    if (service) {
      service->Load();
    }
    return result;
  }
  const TemplateURL* default_engine = service->GetDefaultSearchProvider();
  result->managed =
      service->is_default_search_managed() ||
      (default_engine &&
       default_engine->type() == TemplateURL::NORMAL_CONTROLLED_BY_EXTENSION);
  for (TemplateURL* engine : service->GetTemplateURLs()) {
    const bool is_default = engine == default_engine;
    // The ones upstream's search settings offer: those in the default list
    // that can be the default.
    if (!is_default && (!service->ShowInDefaultList(engine) ||
                        !service->CanMakeDefault(engine))) {
      continue;
    }
    result->engines.push_back(sidebar::mojom::SearchEngine::New(
        base::NumberToString(engine->id().value()),
        base::UTF16ToUTF8(engine->short_name()), is_default));
  }
  return result;
}

void SettingsWindowHandler::GetSearchEngines(
    GetSearchEnginesCallback callback) {
  std::move(callback).Run(DescribeSearchEngines());
}

void SettingsWindowHandler::SetDefaultSearchEngine(const std::string& id) {
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(profile_);
  int64_t engine_id = 0;
  if (!service || !base::StringToInt64(id, &engine_id)) {
    return;
  }
  TemplateURL* engine =
      service->GetTemplateURLForId(TemplateURLID::FromUnsafeValue(engine_id));
  if (!engine || !service->CanMakeDefault(engine)) {
    return;
  }
  service->SetUserSelectedDefaultSearchProvider(
      engine, search_engines::ChoiceMadeLocation::kSearchSettings);
}

void SettingsWindowHandler::OnTemplateURLServiceChanged() {
  page_->SearchEnginesChanged(DescribeSearchEngines());
}

// Downloads
// --------------------------------------------------------------------

void SettingsWindowHandler::GetDownloadLocation(
    GetDownloadLocationCallback callback) {
  base::FilePath path =
      DownloadPrefs::FromBrowserContext(profile_)->DownloadPath();
  // The home folder as ~, as the system writes it.
  base::FilePath home;
  std::string text = path.AsUTF8Unsafe();
  if (base::PathService::Get(base::DIR_HOME, &home) && home.IsParent(path)) {
    base::FilePath relative;
    home.AppendRelativePath(path, &relative);
    text = "~/" + relative.AsUTF8Unsafe();
  }
  std::move(callback).Run(text);
}

void SettingsWindowHandler::ChooseDownloadLocation() {
  if (folder_dialog_ || !web_contents_) {
    return;
  }
  folder_dialog_ = ui::SelectFileDialog::Create(
      this, std::make_unique<ChromeSelectFilePolicy>(web_contents_.get()));
  ui::SelectFileDialog::FileTypeInfo info;
  info.allowed_paths = ui::SelectFileDialog::FileTypeInfo::NATIVE_PATH;
  folder_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_FOLDER, u"Download Location",
      DownloadPrefs::FromBrowserContext(profile_)->DownloadPath(), &info, 0,
      base::FilePath::StringType(), web_contents_->GetTopLevelNativeWindow());
}

void SettingsWindowHandler::FileSelected(const ui::SelectedFileInfo& file,
                                         int index) {
  folder_dialog_.reset();
  // As upstream's DownloadsHandler: downloads and Save As go there.
  PrefService* prefs = profile_->GetPrefs();
  prefs->SetFilePath(::prefs::kDownloadDefaultDirectory, file.path());
  prefs->SetFilePath(::prefs::kSaveFileDefaultDirectory, file.path());
}

void SettingsWindowHandler::FileSelectionCanceled() {
  folder_dialog_.reset();
}

// Fonts and system
// --------------------------------------------------------------

void SettingsWindowHandler::GetFonts(GetFontsCallback callback) {
  content::GetFontListAsync(base::BindOnce(
      [](GetFontsCallback callback, base::ListValue list) {
        std::vector<std::vector<std::string>> fonts;
        for (const base::Value& font : list) {
          const base::ListValue* entry = font.GetIfList();
          if (entry && entry->size() >= 2 && (*entry)[0].is_string() &&
              (*entry)[1].is_string()) {
            fonts.push_back({(*entry)[0].GetString(), (*entry)[1].GetString()});
          }
        }
        std::move(callback).Run(std::move(fonts));
      },
      std::move(callback)));
}

void SettingsWindowHandler::ShowProxySettings() {
  if (web_contents_) {
    settings_utils::ShowNetworkProxySettings(web_contents_);
  }
}

// This profile
// -------------------------------------------------------------------

sidebar::mojom::ProfileDetailsPtr SettingsWindowHandler::DescribeProfile() {
  auto details = sidebar::mojom::ProfileDetails::New();
  ProfileAttributesEntry* entry =
      g_browser_process->profile_manager()
          ->GetProfileAttributesStorage()
          .GetProfileAttributesWithPath(
              profile_->GetOriginalProfile()->GetPath());
  if (entry) {
    details->name = base::UTF16ToUTF8(entry->GetLocalProfileName());
    gfx::Image icon = profiles::GetSizedAvatarIcon(
        entry->GetAvatarIcon(), kProfileIconSize, kProfileIconSize);
    details->icon_url = webui::GetBitmapDataUrl(icon.AsBitmap());
  }
  return details;
}

std::vector<sidebar::mojom::ProfileIconPtr>
SettingsWindowHandler::DescribeProfileIcons() {
  ProfileAttributesEntry* entry =
      g_browser_process->profile_manager()
          ->GetProfileAttributesStorage()
          .GetProfileAttributesWithPath(
              profile_->GetOriginalProfile()->GetPath());
  const size_t selected = entry && !entry->IsUsingDefaultAvatar()
                              ? entry->GetAvatarIconIndex()
                              : SIZE_MAX;
  std::vector<sidebar::mojom::ProfileIconPtr> icons;
  for (const base::Value& value :
       profiles::GetCustomProfileAvatarIconsAndLabels(selected)) {
    const base::DictValue* icon = value.GetIfDict();
    if (!icon) {
      continue;
    }
    const std::string* url = icon->FindString("url");
    const std::string* label = icon->FindString("label");
    std::optional<int> index = icon->FindInt("index");
    if (url && label && index) {
      icons.push_back(sidebar::mojom::ProfileIcon::New(
          *index, *url, *label, icon->FindBool("selected").value_or(false)));
    }
  }
  return icons;
}

void SettingsWindowHandler::GetProfile(GetProfileCallback callback) {
  std::move(callback).Run(DescribeProfile(), DescribeProfileIcons());
}

void SettingsWindowHandler::SetProfileName(const std::string& name) {
  std::u16string trimmed;
  base::TrimWhitespace(base::UTF8ToUTF16(name), base::TRIM_ALL, &trimmed);
  if (trimmed.empty()) {
    return;
  }
  profiles::UpdateProfileName(profile_->GetOriginalProfile(), trimmed);
}

void SettingsWindowHandler::SetProfileIcon(int32_t index) {
  if (index < 0) {
    return;
  }
  profiles::SetDefaultProfileAvatarIndex(
      profile_->GetOriginalProfile(), profiles::GetSanitizedAvatarIndex(index));
}

void SettingsWindowHandler::OnProfileNameChanged(
    const base::FilePath& profile_path,
    const std::u16string& old_profile_name) {
  if (profile_path == profile_->GetOriginalProfile()->GetPath()) {
    page_->ProfileChanged(DescribeProfile(), DescribeProfileIcons());
  }
}

void SettingsWindowHandler::OnProfileAvatarChanged(
    const base::FilePath& profile_path) {
  if (profile_path == profile_->GetOriginalProfile()->GetPath()) {
    page_->ProfileChanged(DescribeProfile(), DescribeProfileIcons());
  }
}

}  // namespace compass
