// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_WINDOW_HANDLER_H_
#define COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_WINDOW_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/extensions/api/passwords_private/passwords_private_delegate.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_attributes_storage_observer.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/common/extensions/api/passwords_private.h"
#include "compass/browser/adblock/adblock_service.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/ui/webui/sidebar/sidebar.mojom.h"
#include "compass/browser/window_sync/space_directory.h"
#include "components/autofill/core/browser/data_manager/personal_data_manager.h"
#include "components/autofill/core/browser/data_manager/personal_data_manager_observer.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/password_manager/core/browser/ui/saved_passwords_presenter.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/search_engines/template_url_service.h"
#include "components/search_engines/template_url_service_observer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class PrefService;
class Profile;
class KeywordEditorController;
class ProfileResetter;

namespace content {
class WebContents;
}  // namespace content

namespace compass {

// The browser side of the settings window's page (chrome://compass-settings,
// docs/design/settings-window.md): its accent (the profile's active space's
// color), its height, links out of it, the paths the window is opened at
// while open, and the prefs its rebuilt panes show (an allowlist, instead of
// settingsPrivate).
class SettingsWindowHandler
    : public sidebar::mojom::SettingsWindowHandler,
      public SpaceList::Observer,
      public SpaceDirectory::Observer,
      public TemplateURLServiceObserver,
      public ProfileAttributesStorageObserver,
      public content_settings::Observer,
      public password_manager::SavedPasswordsPresenter::Observer,
      public autofill::PersonalDataManagerObserver,
      public ui::SelectFileDialog::Listener,
      public adblock::AdBlockService::Observer {
 public:
  SettingsWindowHandler(
      Profile* profile,
      content::WebContents* web_contents,
      mojo::PendingRemote<sidebar::mojom::SettingsWindowPage> page,
      mojo::PendingReceiver<sidebar::mojom::SettingsWindowHandler> receiver);
  SettingsWindowHandler(const SettingsWindowHandler&) = delete;
  SettingsWindowHandler& operator=(const SettingsWindowHandler&) = delete;
  ~SettingsWindowHandler() override;

  // sidebar::mojom::SettingsWindowHandler:
  void GetAccent(GetAccentCallback callback) override;
  void SetContentHeight(int32_t height) override;
  void OpenUrl(const GURL& url) override;
  void Close() override;
  void GetPrefs(const std::vector<std::string>& names,
                GetPrefsCallback callback) override;
  void SetPref(const std::string& name,
               const std::string& value_json,
               SetPrefCallback callback) override;
  void GetDefaultZoom(GetDefaultZoomCallback callback) override;
  void SetDefaultZoom(double factor) override;
  void Relaunch() override;
  void GetDefaultBrowser(GetDefaultBrowserCallback callback) override;
  void SetAsDefaultBrowser() override;
  void GetSearchEngines(GetSearchEnginesCallback callback) override;
  void SetDefaultSearchEngine(const std::string& id) override;
  void GetSearchEngineDetails(GetSearchEngineDetailsCallback callback) override;
  void SaveSearchEngine(const std::string& id,
                        const std::string& name,
                        const std::string& keyword,
                        const std::string& url,
                        SaveSearchEngineCallback callback) override;
  void RemoveSearchEngine(const std::string& id) override;
  void SetSearchEngineActive(const std::string& id, bool active) override;
  void GetLanguages(GetLanguagesCallback callback) override;
  void AddLanguage(const std::string& code) override;
  void RemoveLanguage(const std::string& code) override;
  void MoveLanguage(const std::string& code, bool up) override;
  void SetTranslateOffered(const std::string& code, bool offered) override;
  void GetTranslationPacks(GetTranslationPacksCallback callback) override;
  void SetTranslationDownloadsAllowed(bool allowed) override;
  void RemoveTranslationPack(const std::string& code) override;
  void RemoveAllTranslationPacks() override;
  void GetDownloadLocation(GetDownloadLocationCallback callback) override;
  void ChooseDownloadLocation() override;
  void GetFonts(GetFontsCallback callback) override;
  void ShowProxySettings() override;
  void GetProfiles(GetProfilesCallback callback) override;
  void SetProfileName(const std::string& profile_id,
                      const std::string& name) override;
  void CreateProfile() override;
  void DeleteProfile(const std::string& profile_id) override;
  void GetSpaces(GetSpacesCallback callback) override;
  void AddSpace(const std::string& profile_id) override;
  void RenameSpace(const std::string& id, const std::string& name) override;
  void SetSpaceColor(const std::string& id,
                     sidebar::mojom::SpaceColorPtr color) override;
  void SetSpaceGradient(
      const std::string& id,
      std::vector<sidebar::mojom::SpaceColorPtr> gradient) override;
  void SetSpaceIntensity(const std::string& id, double intensity) override;
  void SetSpaceGrain(const std::string& id, double grain) override;
  void SetSpaceTranslucency(const std::string& id,
                            double translucency) override;
  void SetSpaceAppearance(const std::string& id,
                          sidebar::mojom::SpaceAppearance appearance) override;
  void SetSpaceIcon(const std::string& id,
                    sidebar::mojom::SpaceIconPtr icon) override;
  void SetSpaceProfile(const std::string& id,
                       const std::optional<std::string>& profile_id) override;
  void SetArchiveAfter(const std::string& space_id, uint32_t hours) override;
  void SetSpaceSynced(const std::string& space_id, bool synced) override;
  void DeleteSpace(const std::string& id) override;
  void MoveSpace(const std::string& id, uint32_t index) override;
  void ExportSpaces(const std::optional<std::string>& space_id) override;
  void GetPermissionDefaults(GetPermissionDefaultsCallback callback) override;
  void SetPermissionDefault(const std::string& type,
                            sidebar::mojom::PermissionValue value) override;
  void GetSites(GetSitesCallback callback) override;
  void GetSitePermissions(const std::string& origin,
                          GetSitePermissionsCallback callback) override;
  void SetSitePermission(const std::string& origin,
                         const std::string& type,
                         sidebar::mojom::PermissionValue value) override;
  void ResetSitePermissions(const std::string& origin) override;
  void ClearSiteData(const std::string& origin,
                     ClearSiteDataCallback callback) override;
  void ClearBrowsingData(sidebar::mojom::ClearBrowsingDataOptionsPtr options,
                         ClearBrowsingDataCallback callback) override;
  void SetSafeBrowsingLevel(int32_t level) override;
  void GetSafetyCheck(GetSafetyCheckCallback callback) override;
  void GetFilterLists(GetFilterListsCallback callback) override;
  void SetFilterListEnabled(const std::string& id, bool enabled) override;
  void AddFilterList(const std::string& url,
                     AddFilterListCallback callback) override;
  void RemoveFilterList(const std::string& id) override;
  void UpdateFilterListsNow() override;
  void GetPasswords(GetPasswordsCallback callback) override;
  void RevealPassword(int32_t id, RevealPasswordCallback callback) override;
  void CopyPassword(int32_t id, CopyPasswordCallback callback) override;
  void RemovePassword(int32_t id) override;
  void GetCards(GetCardsCallback callback) override;
  void RemoveCard(const std::string& guid) override;
  void GetAddresses(GetAddressesCallback callback) override;
  void RemoveAddress(const std::string& guid) override;
  void GetCard(const std::string& guid, GetCardCallback callback) override;
  void SaveCard(sidebar::mojom::CardDraftPtr card,
                SaveCardCallback callback) override;
  void GetAddress(const std::string& guid,
                  GetAddressCallback callback) override;
  void SaveAddress(sidebar::mojom::AddressDraftPtr address,
                   SaveAddressCallback callback) override;
  void GetAbout(GetAboutCallback callback) override;
  void CheckForUpdates() override;
  void InstallUpdate() override;
  void ResetSettings(ResetSettingsCallback callback) override;

  // password_manager::SavedPasswordsPresenter::Observer:
  void OnSavedPasswordsChanged(
      const password_manager::PasswordStoreChangeList& changes) override;

  // autofill::PersonalDataManagerObserver:
  void OnPersonalDataChanged() override;

  // content_settings::Observer:
  void OnContentSettingChanged(
      const ContentSettingsPattern& primary_pattern,
      const ContentSettingsPattern& secondary_pattern,
      ContentSettingsTypeSet content_type_set) override;

  // TemplateURLServiceObserver:
  void OnTemplateURLServiceChanged() override;

  // ProfileAttributesStorageObserver:
  void OnProfileAdded(const base::FilePath& profile_path) override;
  void OnProfileWasRemoved(const base::FilePath& profile_path,
                           const std::u16string& profile_name) override;
  void OnProfileNameChanged(const base::FilePath& profile_path,
                            const std::u16string& old_profile_name) override;
  void OnProfileIsOmittedChanged(const base::FilePath& profile_path) override;

  // SpaceDirectory::Observer:
  void OnDirectoryChanged() override;

  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

  // SpaceList::Observer (the window's own profile's spaces: its accent, and
  // the Spaces pane without the directory):
  void OnSpaceAdded(SpaceModel& space) override;
  void OnSpaceUpdated(const SpaceModel& space) override;
  void OnSpaceRemoved() override;
  void OnSpaceMoved(const SpaceModel& space) override;
  void OnActiveSpaceChanged() override;

  // adblock::AdBlockService::Observer:
  void OnFilterListsChanged() override;

 private:
  SpaceList* GetSpaceList();
  sidebar::mojom::SpaceColorPtr GetAccentColor();
  void ShowPath(const std::string& path);
  // The service holding |name| (the profile's, or Local State), if it's on
  // the allowlist.
  PrefService* PrefServiceFor(const std::string& name);
  sidebar::mojom::SettingsPrefPtr DescribePref(const std::string& name);
  void OnPrefChanged(const std::string& name);
  // settings_window_handler_general.cc.
  sidebar::mojom::SearchEnginesPtr DescribeSearchEngines();
  // The search engines' rules, once the service has loaded; null before.
  KeywordEditorController* GetKeywordEditor();
  // settings_window_handler_profiles.cc. Every profile, and every space:
  // the directory's, or the profile's own without one (a profile that can't
  // be listed).
  std::vector<sidebar::mojom::ProfileDetailsPtr> DescribeProfiles();
  std::vector<SpaceDirectory::Entry> GetAllSpaces();
  void SendProfilesAndSpaces();
  SpaceDirectory::Entry FindAnySpace(const std::string& id);
  // The space with |id|, of any profile, unless it's a viewer's.
  SpaceModel* FindEditableSpace(const std::string& id);
  SpaceDirectory* directory();
  // The profiles a space can be assigned to: none without the directory.
  std::vector<sidebar::mojom::ProfileOptionPtr> GetSpaceProfileOptions();
  // settings_window_handler_passwords.cc. The password manager's, made on
  // first use.
  extensions::PasswordsPrivateDelegate* GetPasswordsDelegate();
  autofill::PersonalDataManager* GetPersonalDataManager();
  void OnDefaultBrowserState(
      base::OnceCallback<void(sidebar::mojom::DefaultBrowserStatePtr)> callback,
      shell_integration::DefaultWebClientState state);

  const raw_ptr<Profile> profile_;
  const raw_ptr<content::WebContents> web_contents_;
  mojo::Remote<sidebar::mojom::SettingsWindowPage> page_;
  mojo::Receiver<sidebar::mojom::SettingsWindowHandler> receiver_;
  // The open file dialog, at most one: the download folder, or where an
  // export goes.
  scoped_refptr<ui::SelectFileDialog> folder_dialog_;
  // What folder_dialog_ is choosing for: an export's bookmarks file when
  // set, the download folder otherwise.
  std::optional<std::string> pending_export_;
  scoped_refptr<extensions::PasswordsPrivateDelegate> passwords_delegate_;
  std::unique_ptr<ProfileResetter> resetter_;
  // Manage search engines' rules (made once the service has loaded).
  std::unique_ptr<KeywordEditorController> keyword_editor_;
  // Where each listed password is stored, by id, to remove it from there.
  std::map<int, extensions::api::passwords_private::PasswordStoreSet>
      password_stores_;
  base::ScopedObservation<password_manager::SavedPasswordsPresenter,
                          password_manager::SavedPasswordsPresenter::Observer>
      saved_passwords_observation_{this};
  base::ScopedObservation<autofill::PersonalDataManager,
                          autofill::PersonalDataManagerObserver>
      personal_data_observation_{this};
  base::ScopedObservation<HostContentSettingsMap, content_settings::Observer>
      content_settings_observation_{this};
  base::ScopedObservation<TemplateURLService, TemplateURLServiceObserver>
      template_url_observation_{this};
  base::ScopedObservation<ProfileAttributesStorage,
                          ProfileAttributesStorageObserver>
      profile_attributes_observation_{this};
  PrefChangeRegistrar profile_prefs_;
  PrefChangeRegistrar language_prefs_;
  PrefChangeRegistrar local_prefs_;
  // Page translation's consent and installed packs (local state).
  PrefChangeRegistrar translation_prefs_;
  base::ScopedObservation<SpaceList, SpaceList::Observer> space_observation_{
      this};
  // Whether it observes the directory, which can go first (with the profile
  // manager, at shutdown).
  bool uses_directory_ = false;
  base::ScopedObservation<adblock::AdBlockService,
                          adblock::AdBlockService::Observer>
      adblock_observation_{this};
  base::WeakPtrFactory<SettingsWindowHandler> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_WINDOW_HANDLER_H_
