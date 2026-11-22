// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/settings/settings_window_handler.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profile_resetter/profile_resetter.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/search_engines/keyword_editor_controller.h"
#include "chrome/browser/ui/zoom/chrome_zoom_level_prefs.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/settings_window/settings_window.h"
#include "compass/browser/updates/update_hooks.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/autofill/core/common/autofill_prefs.h"
#include "components/browsing_data/core/pref_names.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/language/core/browser/pref_names.h"
#include "components/live_caption/pref_names.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/performance_manager/public/user_tuning/prefs.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "components/spellcheck/browser/pref_names.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass {

namespace {

// The prefs the window's panes may read and write, with their types. Local
// State's are browser-wide (docs/design/settings-window.md, "Prefs").
struct AllowedPref {
  const char* name;
  base::Value::Type type;
  bool local_state;
};

constexpr AllowedPref kAllowedPrefs[] = {
    // General › Downloads.
    {::prefs::kDownloadDefaultDirectory, base::Value::Type::STRING, false},
    {::prefs::kPromptForDownload, base::Value::Type::BOOLEAN, false},
    // General › Appearance and fonts.
    {::prefs::kWebKitDefaultFontSize, base::Value::Type::INTEGER, false},
    {::prefs::kWebKitDefaultFixedFontSize, base::Value::Type::INTEGER, false},
    {::prefs::kWebKitMinimumFontSize, base::Value::Type::INTEGER, false},
    {"webkit.webprefs.fonts.standard.Zyyy", base::Value::Type::STRING, false},
    {"webkit.webprefs.fonts.serif.Zyyy", base::Value::Type::STRING, false},
    {"webkit.webprefs.fonts.sansserif.Zyyy", base::Value::Type::STRING, false},
    {"webkit.webprefs.fonts.fixed.Zyyy", base::Value::Type::STRING, false},
    // General › Languages.
    {::language::prefs::kAcceptLanguages, base::Value::Type::STRING, false},
    {::translate::prefs::kOfferTranslateEnabled, base::Value::Type::BOOLEAN,
     false},
    {::spellcheck::prefs::kSpellCheckEnable, base::Value::Type::BOOLEAN, false},
    // General › System.
    {::prefs::kHardwareAccelerationModeEnabled, base::Value::Type::BOOLEAN,
     true},
    // Privacy.
    {::prefs::kCookieControlsMode, base::Value::Type::INTEGER, false},
    {::prefs::kEnableDoNotTrack, base::Value::Type::BOOLEAN, false},
    {::prefs::kHttpsOnlyModeEnabled, base::Value::Type::BOOLEAN, false},
    {::prefs::kDnsOverHttpsMode, base::Value::Type::STRING, true},
    // Read here; set with SetSafeBrowsingLevel(), which keeps them coherent.
    {::prefs::kSafeBrowsingEnabled, base::Value::Type::BOOLEAN, false},
    {::prefs::kSafeBrowsingEnhanced, base::Value::Type::BOOLEAN, false},
    // Passwords.
    {::password_manager::prefs::kCredentialsEnableService,
     base::Value::Type::BOOLEAN, false},
    {::password_manager::prefs::kCredentialsEnableAutosignin,
     base::Value::Type::BOOLEAN, false},
    {::autofill::prefs::kAutofillCreditCardEnabled, base::Value::Type::BOOLEAN,
     false},
    {::autofill::prefs::kAutofillProfileEnabled, base::Value::Type::BOOLEAN,
     false},
    // Compass's own: picture-in-picture when the window is left (General),
    // tracking left out of copied links (Privacy).
    {compass::prefs::kPipOnLeavingWindow, base::Value::Type::BOOLEAN, false},
    {compass::prefs::kRemoveTrackingFromCopiedLinks, base::Value::Type::BOOLEAN,
     false},
    // The ad blocker's switch (Privacy), for every profile.
    {compass::adblock::prefs::kEnabled, base::Value::Type::BOOLEAN, true},
    // Advanced.
    {::performance_manager::user_tuning::prefs::kMemorySaverModeState,
     base::Value::Type::INTEGER, true},
    {::performance_manager::user_tuning::prefs::kBatterySaverModeState,
     base::Value::Type::INTEGER, true},
    {::prefs::kLiveCaptionEnabled, base::Value::Type::BOOLEAN, false},
    {::prefs::kAccessibilityFocusHighlightEnabled, base::Value::Type::BOOLEAN,
     false},
    {::prefs::kCaretBrowsingEnabled, base::Value::Type::BOOLEAN, false},
    {updates::kUpdateChannelPref, base::Value::Type::STRING, true},
    // Privacy › Clear browsing data: the choices, kept for next time.
    {::browsing_data::prefs::kDeleteTimePeriod, base::Value::Type::INTEGER,
     false},
    {::browsing_data::prefs::kDeleteBrowsingHistory, base::Value::Type::BOOLEAN,
     false},
    {::browsing_data::prefs::kDeleteDownloadHistory, base::Value::Type::BOOLEAN,
     false},
    {::browsing_data::prefs::kDeleteCookies, base::Value::Type::BOOLEAN, false},
    {::browsing_data::prefs::kDeleteCache, base::Value::Type::BOOLEAN, false},
    {::browsing_data::prefs::kDeletePasswords, base::Value::Type::BOOLEAN,
     false},
    {::browsing_data::prefs::kDeleteFormData, base::Value::Type::BOOLEAN,
     false},
    {::browsing_data::prefs::kDeleteSiteSettings, base::Value::Type::BOOLEAN,
     false},
};

// Whether |value| (of the pref's type) is one the pref takes.
bool IsValidValue(std::string_view name, const base::Value& value) {
  if (name == updates::kUpdateChannelPref) {
    const std::string& channel = value.GetString();
    return channel == updates::kChannelStable ||
           channel == updates::kChannelBeta ||
           channel == updates::kChannelCanary;
  }
  if (name ==
      ::performance_manager::user_tuning::prefs::kMemorySaverModeState) {
    // Off or on (the middle value is deprecated).
    return value.GetInt() == 0 || value.GetInt() == 2;
  }
  if (name ==
      ::performance_manager::user_tuning::prefs::kBatterySaverModeState) {
    return value.GetInt() >= 0 && value.GetInt() <= 3;
  }
  if (name == ::prefs::kCookieControlsMode) {
    return value.GetInt() >= 0 && value.GetInt() <= 2;
  }
  if (name == ::prefs::kDnsOverHttpsMode) {
    // The window only turns it on (automatic) or off; a custom provider is
    // upstream's to set.
    return value.GetString() == "off" || value.GetString() == "automatic";
  }
  return true;
}

// Set only through their own methods.
bool IsReadOnly(std::string_view name) {
  return name == ::prefs::kSafeBrowsingEnabled ||
         name == ::prefs::kSafeBrowsingEnhanced;
}

const AllowedPref* FindAllowed(std::string_view name) {
  for (const AllowedPref& pref : kAllowedPrefs) {
    if (name == pref.name) {
      return &pref;
    }
  }
  return nullptr;
}

}  // namespace

SettingsWindowHandler::SettingsWindowHandler(
    Profile* profile,
    content::WebContents* web_contents,
    mojo::PendingRemote<sidebar::mojom::SettingsWindowPage> page,
    mojo::PendingReceiver<sidebar::mojom::SettingsWindowHandler> receiver)
    : profile_(profile),
      web_contents_(web_contents),
      page_(std::move(page)),
      receiver_(this, std::move(receiver)) {
  if (SpaceList* list = GetSpaceList()) {
    space_observation_.Observe(list);
  }
  if (adblock::AdBlockService* adblock = adblock::AdBlockService::Get()) {
    adblock_observation_.Observe(adblock);
  }
  profile_prefs_.Init(profile_->GetPrefs());
  local_prefs_.Init(g_browser_process->local_state());
  language_prefs_.Init(profile_->GetPrefs());
  // The languages' prefs, for General › Languages.
  for (const char* name : {::language::prefs::kAcceptLanguages,
                           ::translate::prefs::kBlockedLanguages}) {
    language_prefs_.Add(name, base::BindRepeating(
                                  [](SettingsWindowHandler* handler) {
                                    handler->page_->LanguagesChanged();
                                  },
                                  base::Unretained(this)));
  }
  if (HostContentSettingsMap* map =
          HostContentSettingsMapFactory::GetForProfile(profile_)) {
    content_settings_observation_.Observe(map);
  }
  if (TemplateURLService* service =
          TemplateURLServiceFactory::GetForProfile(profile_)) {
    template_url_observation_.Observe(service);
  }
  if (autofill::PersonalDataManager* data = GetPersonalDataManager()) {
    personal_data_observation_.Observe(data);
  }
  if (ProfileManager* manager = g_browser_process->profile_manager()) {
    profile_attributes_observation_.Observe(
        &manager->GetProfileAttributesStorage());
  }
  if (SpaceDirectory::IsListable(profile_->GetOriginalProfile())) {
    if (SpaceDirectory* directory = SpaceDirectory::Get()) {
      uses_directory_ = true;
      directory->AddObserver(this);
    }
  }
  if (SettingsWindow* window = SettingsWindow::FromWebContents(web_contents_)) {
    window->SetShowPathCallback(base::BindRepeating(
        &SettingsWindowHandler::ShowPath, weak_factory_.GetWeakPtr()));
  }
}

SettingsWindowHandler::~SettingsWindowHandler() {
  if (SpaceDirectory* directory = this->directory()) {
    directory->RemoveObserver(this);
  }
  if (folder_dialog_) {
    folder_dialog_->ListenerDestroyed();
  }
  if (SettingsWindow* window = SettingsWindow::FromWebContents(web_contents_)) {
    window->SetShowPathCallback({});
  }
}

SpaceList* SettingsWindowHandler::GetSpaceList() {
  // The spaces are the original profile's.
  Profile* profile = profile_ ? profile_->GetOriginalProfile() : nullptr;
  SpaceService* service = profile ? SpaceService::Get(profile) : nullptr;
  return service ? &service->space_list() : nullptr;
}

sidebar::mojom::SpaceColorPtr SettingsWindowHandler::GetAccentColor() {
  SpaceList* list = GetSpaceList();
  if (!list) {
    return nullptr;
  }
  const SpaceColor color = list->active_space().color();
  if (color.is_grey()) {
    return nullptr;
  }
  const SkColor tint = *color.tint();
  return sidebar::mojom::SpaceColor::New(SkColorGetR(tint), SkColorGetG(tint),
                                         SkColorGetB(tint));
}

void SettingsWindowHandler::GetAccent(GetAccentCallback callback) {
  std::move(callback).Run(GetAccentColor());
}

void SettingsWindowHandler::SetContentHeight(int32_t height) {
  if (SettingsWindow* window = SettingsWindow::FromWebContents(web_contents_)) {
    window->SetContentHeight(height);
  }
}

void SettingsWindowHandler::OpenUrl(const GURL& url) {
  // Only browser pages: the page links to the password manager and such.
  if (!url.SchemeIs(content::kChromeUIScheme)) {
    return;
  }
  NavigateParams params(profile_.get(), url, ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  params.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&params);
}

void SettingsWindowHandler::Close() {
  if (SettingsWindow* window = SettingsWindow::FromWebContents(web_contents_)) {
    window->Close();
  }
}

void SettingsWindowHandler::OnSpaceAdded(SpaceModel& space) {
  // The directory tells of every profile's.
  if (!directory()) {
    SendProfilesAndSpaces();
  }
}

void SettingsWindowHandler::OnSpaceUpdated(const SpaceModel& space) {
  SpaceList* list = GetSpaceList();
  if (list && &list->active_space() == &space) {
    page_->SetAccent(GetAccentColor());
  }
  if (!directory()) {
    SendProfilesAndSpaces();
  }
}

void SettingsWindowHandler::OnSpaceRemoved() {
  if (!directory()) {
    SendProfilesAndSpaces();
  }
}

void SettingsWindowHandler::OnSpaceMoved(const SpaceModel& space) {
  if (!directory()) {
    SendProfilesAndSpaces();
  }
}

void SettingsWindowHandler::OnActiveSpaceChanged() {
  page_->SetAccent(GetAccentColor());
}

void SettingsWindowHandler::ShowPath(const std::string& path) {
  page_->ShowPath(path);
}

// Prefs -----------------------------------------------------------------------

PrefService* SettingsWindowHandler::PrefServiceFor(const std::string& name) {
  const AllowedPref* allowed = FindAllowed(name);
  if (!allowed) {
    return nullptr;
  }
  return allowed->local_state ? g_browser_process->local_state()
                              : profile_->GetPrefs();
}

sidebar::mojom::SettingsPrefPtr SettingsWindowHandler::DescribePref(
    const std::string& name) {
  PrefService* service = PrefServiceFor(name);
  const PrefService::Preference* pref =
      service ? service->FindPreference(name) : nullptr;
  if (!pref) {
    return nullptr;
  }
  return sidebar::mojom::SettingsPref::New(
      name, base::WriteJson(*pref->GetValue()).value_or("null"),
      pref->IsManaged() || pref->IsExtensionControlled() ||
          !pref->IsUserModifiable());
}

void SettingsWindowHandler::GetPrefs(const std::vector<std::string>& names,
                                     GetPrefsCallback callback) {
  std::vector<sidebar::mojom::SettingsPrefPtr> result;
  for (const std::string& name : names) {
    sidebar::mojom::SettingsPrefPtr pref = DescribePref(name);
    if (!pref) {
      continue;
    }
    PrefChangeRegistrar& registrar =
        FindAllowed(name)->local_state ? local_prefs_ : profile_prefs_;
    if (!registrar.IsObserved(name)) {
      registrar.Add(name,
                    base::BindRepeating(&SettingsWindowHandler::OnPrefChanged,
                                        base::Unretained(this), name));
    }
    result.push_back(std::move(pref));
  }
  std::move(callback).Run(std::move(result));
}

void SettingsWindowHandler::SetPref(const std::string& name,
                                    const std::string& value_json,
                                    SetPrefCallback callback) {
  const AllowedPref* allowed = FindAllowed(name);
  PrefService* service = PrefServiceFor(name);
  const PrefService::Preference* pref =
      service ? service->FindPreference(name) : nullptr;
  std::optional<base::Value> value =
      base::JSONReader::Read(value_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!allowed || !pref || !value || value->type() != allowed->type ||
      !pref->IsUserModifiable() || IsReadOnly(name) ||
      !IsValidValue(name, *value)) {
    std::move(callback).Run(false);
    return;
  }
  service->Set(name, std::move(*value));
  std::move(callback).Run(true);
}

void SettingsWindowHandler::OnPrefChanged(const std::string& name) {
  if (sidebar::mojom::SettingsPrefPtr pref = DescribePref(name)) {
    page_->PrefChanged(std::move(pref));
  }
}

void SettingsWindowHandler::GetDefaultZoom(GetDefaultZoomCallback callback) {
  // No zoom prefs off the record.
  if (profile_->IsOffTheRecord()) {
    std::move(callback).Run(1.0);
    return;
  }
  std::move(callback).Run(blink::ZoomLevelToZoomFactor(
      profile_->GetZoomLevelPrefs()->GetDefaultZoomLevelPref()));
}

void SettingsWindowHandler::SetDefaultZoom(double factor) {
  if (profile_->IsOffTheRecord() || factor < 0.25 || factor > 5) {
    return;
  }
  profile_->GetZoomLevelPrefs()->SetDefaultZoomLevelPref(
      blink::ZoomFactorToZoomLevel(factor));
}

void SettingsWindowHandler::Relaunch() {
  chrome::AttemptRestart();
}

}  // namespace compass
