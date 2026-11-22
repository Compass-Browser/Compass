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
#include "base/i18n/language_tag.h"
#include "base/i18n/tag_converters.h"
#include "base/memory/raw_ptr.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/search_engines/ui_thread_search_terms_data.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/ui/search_engines/edit_search_engine_controller.h"
#include "chrome/browser/ui/search_engines/keyword_editor_controller.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "chrome/browser/ui/webui/settings/settings_utils.h"
#include "chrome/common/pref_names.h"
#include "compass/browser/ui/webui/settings/settings_window_handler.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/choice_made_location.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_starter_pack_data.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "content/public/browser/font_list_async.h"
#include "content/public/browser/web_contents.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace compass {

namespace {

bool DefaultBrowserDisabledByPolicy() {
  const PrefService::Preference* pref =
      g_browser_process->local_state()->FindPreference(
          ::prefs::kDefaultBrowserSettingEnabled);
  return pref && pref->IsManaged() && !pref->GetValue()->GetBool();
}

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

// Search engines --------------------------------------------------------------

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

// Manage search engines -------------------------------------------------------

namespace {

// Saves an add or an edit as upstream's SearchEnginesHandler does: through
// the keyword editor, with the URL EditSearchEngineController fixed up.
class SaveSearchEngineDelegate : public EditSearchEngineControllerDelegate {
 public:
  explicit SaveSearchEngineDelegate(KeywordEditorController* editor)
      : editor_(editor) {}
  ~SaveSearchEngineDelegate() override = default;

  void OnEditedKeyword(TemplateURL* template_url,
                       const std::u16string& title,
                       const std::u16string& keyword,
                       const std::string& fixed_up_url) override {
    if (template_url) {
      editor_->ModifyTemplateURL(template_url, title, keyword, fixed_up_url);
    } else {
      editor_->AddTemplateURL(title, keyword, fixed_up_url);
    }
  }

 private:
  raw_ptr<KeywordEditorController> editor_;
};

std::optional<TemplateURLID> ParseEngineId(const std::string& id) {
  int64_t value = 0;
  if (!base::StringToInt64(id, &value)) {
    return std::nullopt;
  }
  return TemplateURLID::FromUnsafeValue(value);
}

}  // namespace

KeywordEditorController* SettingsWindowHandler::GetKeywordEditor() {
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(profile_);
  if (!service || !service->loaded()) {
    if (service) {
      service->Load();
    }
    return nullptr;
  }
  if (!keyword_editor_) {
    keyword_editor_ = std::make_unique<KeywordEditorController>(profile_);
  }
  return keyword_editor_.get();
}

void SettingsWindowHandler::GetSearchEngineDetails(
    GetSearchEngineDetailsCallback callback) {
  std::vector<sidebar::mojom::SearchEngineDetailsPtr> engines;
  KeywordEditorController* editor = GetKeywordEditor();
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(profile_);
  if (!editor || !service) {
    std::move(callback).Run(std::move(engines));
    return;
  }
  const TemplateURL* default_engine = service->GetDefaultSearchProvider();
  UIThreadSearchTermsData search_terms_data;
  for (TemplateURL* engine : service->GetTemplateURLs()) {
    // Not the command bar's built-in shortcuts (@tabs, @history...), nor
    // extensions' omnibox keywords.
    if (engine->starter_pack_id() !=
            template_url_starter_pack_data::StarterPackId::kNone ||
        engine->type() == TemplateURL::OMNIBOX_API_EXTENSION) {
      continue;
    }
    const bool is_default = engine == default_engine;
    std::string url =
        base::UTF16ToUTF8(engine->url_ref().DisplayURL(search_terms_data));
    // %s for the query, as the user types it.
    base::ReplaceSubstringsAfterOffset(&url, 0, "{searchTerms}", "%s");
    engines.push_back(sidebar::mojom::SearchEngineDetails::New(
        base::NumberToString(engine->id().value()),
        base::UTF16ToUTF8(engine->short_name()),
        base::UTF16ToUTF8(engine->keyword()), url, is_default,
        engine->prepopulate_id() == 0 && !is_default,
        engine->is_active() == TemplateURLData::ActiveStatus::kTrue,
        editor->CanEdit(engine), editor->CanRemove(engine),
        editor->CanMakeDefault(engine), editor->CanActivate(engine),
        editor->CanDeactivate(engine), editor->IsManaged(engine)));
  }
  std::move(callback).Run(std::move(engines));
}

void SettingsWindowHandler::SaveSearchEngine(
    const std::string& id,
    const std::string& name,
    const std::string& keyword,
    const std::string& url,
    SaveSearchEngineCallback callback) {
  KeywordEditorController* editor = GetKeywordEditor();
  if (!editor) {
    std::move(callback).Run("Search engines aren't loaded yet.");
    return;
  }
  TemplateURL* engine = nullptr;
  if (!id.empty()) {
    std::optional<TemplateURLID> engine_id = ParseEngineId(id);
    engine = engine_id ? editor->GetTemplateURL(*engine_id) : nullptr;
    if (!engine || !editor->CanEdit(engine)) {
      std::move(callback).Run("This search engine can't be edited.");
      return;
    }
  }
  SaveSearchEngineDelegate delegate(editor);
  EditSearchEngineController controller(engine, &delegate, profile_);
  const std::u16string title = base::UTF8ToUTF16(name);
  const std::u16string shortcut = base::UTF8ToUTF16(keyword);
  if (!controller.IsTitleValid(title)) {
    std::move(callback).Run("Give it a name.");
    return;
  }
  if (!controller.IsKeywordValid(shortcut)) {
    std::move(callback).Run(
        "Use a shortcut without spaces that no other engine has.");
    return;
  }
  if (!controller.IsURLValid(url)) {
    std::move(callback).Run(
        "Use a web address; %s in it marks where the query goes.");
    return;
  }
  controller.AcceptAddOrEdit(title, shortcut, url);
  std::move(callback).Run(std::string());
}

void SettingsWindowHandler::RemoveSearchEngine(const std::string& id) {
  KeywordEditorController* editor = GetKeywordEditor();
  std::optional<TemplateURLID> engine_id = ParseEngineId(id);
  TemplateURL* engine =
      editor && engine_id ? editor->GetTemplateURL(*engine_id) : nullptr;
  if (engine && editor->CanRemove(engine)) {
    editor->RemoveTemplateURL(*engine_id);
  }
}

void SettingsWindowHandler::SetSearchEngineActive(const std::string& id,
                                                  bool active) {
  KeywordEditorController* editor = GetKeywordEditor();
  std::optional<TemplateURLID> engine_id = ParseEngineId(id);
  TemplateURL* engine =
      editor && engine_id ? editor->GetTemplateURL(*engine_id) : nullptr;
  if (engine &&
      (active ? editor->CanActivate(engine) : editor->CanDeactivate(engine))) {
    editor->SetIsActiveTemplateURL(*engine_id, active);
  }
}

void SettingsWindowHandler::OnTemplateURLServiceChanged() {
  page_->SearchEnginesChanged(DescribeSearchEngines());
}

// Languages -------------------------------------------------------------------

void SettingsWindowHandler::GetLanguages(GetLanguagesCallback callback) {
  std::vector<translate::TranslateLanguageInfo> infos;
  translate::TranslatePrefs::GetLanguageInfoList(
      g_browser_process->GetApplicationLocale(),
      profile_->GetPrefs()->GetBoolean(
          ::translate::prefs::kOfferTranslateEnabled),
      &infos);
  std::vector<sidebar::mojom::LanguageInfoPtr> catalog;
  for (const translate::TranslateLanguageInfo& info : infos) {
    catalog.push_back(sidebar::mojom::LanguageInfo::New(
        info.code, info.display_name, info.native_display_name,
        info.supports_translate));
  }
  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(profile_->GetPrefs());
  std::vector<sidebar::mojom::PreferredLanguagePtr> preferred;
  for (const base::i18n::LanguageTag& tag :
       translate_prefs->GetUserSelectedLanguageList()) {
    const std::string code(tag.tag_string());
    preferred.push_back(sidebar::mojom::PreferredLanguage::New(
        code, !translate_prefs->IsBlockedLanguage(code)));
  }
  std::move(callback).Run(std::move(catalog), std::move(preferred));
}

void SettingsWindowHandler::AddLanguage(const std::string& code) {
  std::optional<base::i18n::LanguageTag> tag =
      base::i18n::GetLanguageTagFromString(code);
  if (tag) {
    ChromeTranslateClient::CreateTranslatePrefs(profile_->GetPrefs())
        ->AddToLanguageList(*tag, /*force_blocked=*/false);
  }
}

void SettingsWindowHandler::RemoveLanguage(const std::string& code) {
  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(profile_->GetPrefs());
  std::optional<base::i18n::LanguageTag> tag =
      base::i18n::GetLanguageTagFromString(code);
  // Never the last one.
  if (tag && translate_prefs->GetUserSelectedLanguageList().size() > 1) {
    translate_prefs->RemoveFromLanguageList(*tag);
  }
}

void SettingsWindowHandler::MoveLanguage(const std::string& code, bool up) {
  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(profile_->GetPrefs());
  std::vector<std::string> enabled;
  for (const base::i18n::LanguageTag& tag :
       translate_prefs->GetUserSelectedLanguageList()) {
    enabled.emplace_back(tag.tag_string());
  }
  translate_prefs->RearrangeLanguage(
      code,
      up ? translate::TranslatePrefs::kUp : translate::TranslatePrefs::kDown,
      /*offset=*/1, enabled);
}

void SettingsWindowHandler::SetTranslateOffered(const std::string& code,
                                                bool offered) {
  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(profile_->GetPrefs());
  if (offered) {
    translate_prefs->UnblockLanguage(code);
  } else {
    translate_prefs->BlockLanguage(code);
  }
}

// Downloads -------------------------------------------------------------------

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

// Fonts and system ------------------------------------------------------------

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

// This profile ----------------------------------------------------------------

sidebar::mojom::ProfileDetailsPtr SettingsWindowHandler::DescribeProfile() {
  auto details = sidebar::mojom::ProfileDetails::New();
  ProfileAttributesEntry* entry =
      g_browser_process->profile_manager()
          ->GetProfileAttributesStorage()
          .GetProfileAttributesWithPath(
              profile_->GetOriginalProfile()->GetPath());
  if (entry) {
    details->name = base::UTF16ToUTF8(entry->GetLocalProfileName());
  }
  return details;
}

void SettingsWindowHandler::GetProfile(GetProfileCallback callback) {
  std::move(callback).Run(DescribeProfile());
}

void SettingsWindowHandler::SetProfileName(const std::string& name) {
  std::u16string trimmed;
  base::TrimWhitespace(base::UTF8ToUTF16(name), base::TRIM_ALL, &trimmed);
  if (trimmed.empty()) {
    return;
  }
  profiles::UpdateProfileName(profile_->GetOriginalProfile(), trimmed);
}

void SettingsWindowHandler::OnProfileNameChanged(
    const base::FilePath& profile_path,
    const std::u16string& old_profile_name) {
  if (profile_path == profile_->GetOriginalProfile()->GetPath()) {
    page_->ProfileChanged(DescribeProfile());
  }
}

}  // namespace compass
