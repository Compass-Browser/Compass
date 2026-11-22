// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/browser_importer.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/version_info/version_info.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/importer/external_process_importer_host.h"
#include "chrome/browser/importer/importer_progress_observer.h"
#include "chrome/browser/importer/profile_writer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_constants.h"
#include "compass/browser/arc_import/profile_copy.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/browser_import/detect.h"
#include "compass/browser/browser_import/profile_reader.h"
#include "compass/browser/browser_import/space_import.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/arc_import/arc_importer.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/history/core/browser/url_row.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/prefs/pref_service.h"
#include "components/user_data_importer/common/importer_data_types.h"
#include "components/user_data_importer/common/importer_type.h"

namespace compass::browser_import {

namespace {

scoped_refptr<base::SequencedTaskRunner> ReadRunner() {
  return base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN});
}

// Copying a profile: blocking, and on shutdown it finishes rather than
// leaving half a profile folder.
scoped_refptr<base::SequencedTaskRunner> CopyRunner() {
  return base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN});
}

// The Compass profiles in |user_data_dir| copied from other browsers'
// profiles, by source profile ID (prefs::kBrowserImportProfile in their
// Preferences). Profiles deleted since may be among them. Blocking.
std::map<std::string, base::FilePath> FindProfilesImportedFromBrowsers(
    const base::FilePath& user_data_dir) {
  std::map<std::string, base::FilePath> imported;
  if (user_data_dir.empty()) {
    return imported;
  }
  base::FileEnumerator dirs(user_data_dir, /*recursive=*/false,
                            base::FileEnumerator::DIRECTORIES);
  for (base::FilePath dir = dirs.Next(); !dir.empty(); dir = dirs.Next()) {
    std::string json;
    if (!base::ReadFileToString(dir.Append(chrome::kPreferencesFilename),
                                &json)) {
      continue;
    }
    std::optional<base::Value> preferences =
        base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    const std::string* from =
        preferences && preferences->is_dict()
            ? preferences->GetDict().FindStringByDottedPath(
                  prefs::kBrowserImportProfile)
            : nullptr;
    if (from && !from->empty()) {
      imported.emplace(*from, dir);
    }
  }
  return imported;
}

// The name of the Compass profile at |path|, if it's still there.
std::optional<std::u16string> LiveProfileName(const base::FilePath& path) {
  ProfileManager* manager = g_browser_process->profile_manager();
  ProfileAttributesEntry* entry =
      manager
          ? manager->GetProfileAttributesStorage().GetProfileAttributesWithPath(
                path)
          : nullptr;
  if (!entry) {
    return std::nullopt;
  }
  return entry->GetLocalProfileName();
}

// A profile found, with the sidebar it would bring.
struct Found {
  ProfileOverview overview;
  // Its sidebar, built on the UI thread (FinishReadingProfile()).
  ProfileData data;
  // The Compass profile it was copied into before, if any.
  base::FilePath imported_as;
};

std::vector<Found> FindProfiles(const base::FilePath& user_data_dir) {
  std::vector<Found> found;
  const std::map<std::string, base::FilePath> as_profiles =
      FindProfilesImportedFromBrowsers(user_data_dir);
  const int compass_version = version_info::GetMajorVersionNumberAsInt();
  for (SourceProfile& profile : DetectProfiles()) {
    Found entry;
    entry.overview.running = IsRunning(profile);
    if (CanImportAsProfile(profile.browser)) {
      entry.overview.as_profile = true;
      const std::optional<int> version = ChromiumMajorVersion(profile.root);
      if (version && *version > compass_version) {
        entry.overview.newer_version = *version;
      }
      if (const auto it = as_profiles.find(profile.id);
          it != as_profiles.end()) {
        entry.imported_as = it->second;
      }
    }
    entry.overview.history = CanImportHistory(profile.browser);
    entry.overview.passwords = CanImportPasswords(profile.browser);
    // Only the sidebar: no Keychain prompt, no history yet.
    entry.data = ReadProfile(profile, ReadOptions());
    if (!entry.data.problems.empty()) {
      entry.overview.note = entry.data.problems.front();
    }
    entry.overview.source = std::move(profile);
    found.push_back(std::move(entry));
  }
  return found;
}

void OnFound(base::WeakPtr<Profile> target,
             base::OnceCallback<void(std::vector<ProfileOverview>)> callback,
             std::vector<Found> found) {
  if (!target) {
    std::move(callback).Run({});
    return;
  }
  SpaceService* service = SpaceService::Get(target.get());
  service->RunWhenLoaded(base::BindOnce(
      [](base::WeakPtr<Profile> target,
         base::OnceCallback<void(std::vector<ProfileOverview>)> callback,
         std::vector<Found> found) {
        std::vector<ProfileOverview> overviews;
        for (Found& entry : found) {
          FinishReadingProfile(entry.data);
          const ImportedSidebar& sidebar = entry.data.sidebar;
          for (const arc::Space& space : sidebar.spaces) {
            entry.overview.spaces.push_back(space.name);
            entry.overview.bookmarks += CountTabs(space.pinned);
            entry.overview.tabs += CountTabs(space.today);
          }
          entry.overview.bookmarks += CountTabs(sidebar.favorites);
          if (!entry.imported_as.empty()) {
            entry.overview.imported_as_profile =
                LiveProfileName(entry.imported_as).value_or(u"");
          }
          if (target) {
            const SpaceList& list =
                SpaceService::Get(target.get())->space_list();
            entry.overview.imported =
                !sidebar.spaces.empty() &&
                std::ranges::all_of(sidebar.spaces,
                                    [&](const arc::Space& space) {
                                      return IsImported(list, space);
                                    });
          }
          overviews.push_back(std::move(entry.overview));
        }
        std::move(callback).Run(std::move(overviews));
      },
      target, std::move(callback), std::move(found)));
}

password_manager::PasswordForm ToPasswordForm(const LoginEntry& login) {
  password_manager::PasswordForm form;
  form.url = login.url;
  form.action = login.action;
  form.signon_realm = login.signon_realm;
  form.username_element = login.username_element;
  form.username_value = login.username;
  form.password_element = login.password_element;
  form.password_value =
      password_manager::PasswordString(std::u16string(login.password));
  form.date_created = login.created;
  form.date_password_modified = login.created;
  form.blocked_by_user = login.blocked;
  if (login.scheme >= 0 &&
      login.scheme <=
          static_cast<int>(password_manager::PasswordForm::Scheme::kMaxValue)) {
    form.scheme = static_cast<password_manager::PasswordForm::Scheme>(
        login.scheme);
  }
  form.type = password_manager::PasswordForm::Type::kImported;
  form.in_store = password_manager::PasswordForm::Store::kProfileStore;
  return form;
}

// A profile of another browser copied into |to| as a Compass profile, with
// the sidebar read from the original (the copy leaves its open tabs out).
struct ProfileCopy {
  ProfileCopy();
  ProfileCopy(ProfileCopy&&);
  ProfileCopy& operator=(ProfileCopy&&);
  ~ProfileCopy();

  arc::CopyResult copy;
  ProfileData data;
  // Why it wasn't copied, in words for the page.
  std::string problem;
};

ProfileCopy::ProfileCopy() = default;
ProfileCopy::ProfileCopy(ProfileCopy&&) = default;
ProfileCopy& ProfileCopy::operator=(ProfileCopy&&) = default;
ProfileCopy::~ProfileCopy() = default;

// Copies |profile| into |to| with its values re-encrypted with Compass's
// |encryptor|, and signs its preferences for Compass (recording where it
// came from), as the Arc import does (docs/design/browser-import.md, "As a
// profile"). Blocking; macOS may ask for the browser's Keychain key.
ProfileCopy CopyAsProfile(const SourceProfile& profile,
                          const ReadOptions& options,
                          const base::FilePath& to,
                          const base::FilePath& user_data_dir,
                          scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  ProfileCopy result;
  if (FindProfilesImportedFromBrowsers(user_data_dir).contains(profile.id)) {
    result.problem = "It was imported as a profile before.";
    return result;
  }
  PasswordDecryptor decrypt =
      ChromiumPasswordDecryptor(profile.browser, profile.root);
  if (!decrypt || !encryptor) {
    result.problem = "It couldn't be copied: Compass wasn't given the key " +
                     base::UTF16ToUTF8(BrowserName(profile.browser)) +
                     " encrypts its cookies and passwords with.";
    return result;
  }
  result.data = ReadProfile(profile, options);
  result.copy = arc::CopyProfile(
      profile.path, to, base::FilePath(), base::FilePath(),
      base::BindRepeating(
          [](const PasswordDecryptor& decrypt,
             const os_crypt_async::Encryptor* encryptor,
             std::string_view value) -> std::optional<std::string> {
            std::optional<std::string> plaintext = decrypt.Run(value);
            std::string ciphertext;
            if (!plaintext ||
                !encryptor->EncryptString(*plaintext, &ciphertext)) {
              return std::nullopt;
            }
            return ciphertext;
          },
          decrypt, base::RetainedRef(encryptor)),
      /*skip_caches=*/true);
  if (result.copy.ok && !arc::SignCopiedPreferences(
                            to, prefs::kBrowserImportProfile, profile.id)) {
    base::DeletePathRecursively(to);
    result.copy = arc::CopyResult();
  }
  if (!result.copy.ok) {
    result.problem =
        "Its folder couldn't be copied. Check that there's enough free "
        "space, then try again.";
  }
  return result;
}

history::VisitSource VisitSourceFor(Browser browser) {
  if (browser == Browser::kSafari) {
    return history::SOURCE_SAFARI_IMPORTED;
  }
  // Chromium has no source for Chromium-based browsers; an "imported" one
  // keeps the visits apart from browsing.
  return history::SOURCE_IE_IMPORTED;
}

// Imports one profile after another, then deletes itself.
class Importer : public importer::ImporterProgressObserver {
 public:
  Importer(Profile* target,
           std::vector<BrowserImportChoice> choices,
           base::OnceCallback<void(BrowserImportResult)> callback)
      : target_(target->GetWeakPtr()),
        choices_(std::move(choices)),
        callback_(std::move(callback)),
        runner_(ReadRunner()) {}

  ~Importer() override {
    if (host_) {
      host_->set_observer(nullptr);
    }
  }

  void Start() {
    runner_->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&DetectProfiles),
        base::BindOnce(&Importer::OnDetected, weak_factory_.GetWeakPtr()));
  }

 private:
  struct Job {
    SourceProfile profile;
    BrowserImportChoice choice;
  };

  void OnDetected(std::vector<SourceProfile> profiles) {
    std::map<std::string, SourceProfile> by_id;
    for (SourceProfile& profile : profiles) {
      by_id[profile.id] = std::move(profile);
    }
    for (const BrowserImportChoice& choice : choices_) {
      const auto it = by_id.find(choice.id);
      if (it == by_id.end()) {
        BrowserImportSummary summary;
        summary.name = base::UTF8ToUTF16(choice.id);
        summary.problems.push_back("This profile wasn't found any more.");
        result_.summaries.push_back(std::move(summary));
        continue;
      }
      jobs_.push_back({it->second, choice});
    }
    Next();
  }

  void Next() {
    if (next_ >= jobs_.size() || !target_) {
      std::move(callback_).Run(std::move(result_));
      delete this;
      return;
    }
    const Job& job = jobs_[next_];
    if (job.choice.new_profile && CanImportAsProfile(job.profile.browser)) {
      g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
          &Importer::CopyWithEncryptor, weak_factory_.GetWeakPtr()));
      return;
    }
    ReadOptions options;
    options.bookmarks = job.choice.bookmarks;
    options.tabs = job.choice.tabs;
    const bool own_history = !ImportsHistoryThroughChromium(job.profile.browser);
    options.history = own_history && job.choice.history &&
                      CanImportHistory(job.profile.browser);
    options.passwords = own_history && job.choice.passwords &&
                        CanImportPasswords(job.profile.browser);
    runner_->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&ReadProfile, job.profile, options),
        base::BindOnce(&Importer::OnRead, weak_factory_.GetWeakPtr()));
  }

  void OnRead(ProfileData data) {
    if (!target_) {
      Next();
      return;
    }
    data_ = std::move(data);
    FinishReadingProfile(data_);
    SpaceService::Get(target_.get())
        ->RunWhenLoaded(base::BindOnce(&Importer::OnSpacesLoaded,
                                       weak_factory_.GetWeakPtr()));
  }

  void OnSpacesLoaded() {
    const Job& job = jobs_[next_];
    BrowserImportSummary summary;
    summary.name = SpaceNameFor(job.profile);
    if (!target_) {
      Finish(std::move(summary));
      return;
    }
    Profile* profile = target_.get();
    AddedSidebar added = AddImportedSidebar(
        SpaceService::Get(profile)->space_list(), data_.sidebar,
        base::Time::Now());
    Summarize(added, *profile, summary);
    if ((job.choice.bookmarks || job.choice.tabs) && added.spaces.empty() &&
        !data_.sidebar.spaces.empty()) {
      summary.problems.push_back(
          "Its spaces were imported before, so they were left as they are.");
    }

    auto writer = base::MakeRefCounted<ProfileWriter>(profile);
    if (!data_.history.empty()) {
      history::URLRows rows;
      for (const HistoryEntry& entry : data_.history) {
        history::URLRow row(entry.url);
        row.set_title(entry.title);
        row.set_visit_count(std::max(entry.visit_count, 1));
        row.set_typed_count(entry.typed_count);
        row.set_last_visit(entry.last_visit.is_null() ? base::Time::Now()
                                                      : entry.last_visit);
        rows.push_back(std::move(row));
      }
      writer->AddHistoryPage(rows, VisitSourceFor(job.profile.browser));
      summary.history = static_cast<int>(rows.size());
    }
    for (const LoginEntry& login : data_.logins) {
      writer->AddPasswordForm(ToPasswordForm(login));
    }
    summary.passwords = static_cast<int>(data_.logins.size());
    summary.passwords_skipped = data_.logins_skipped;
    summary.problems.insert(summary.problems.end(), data_.problems.begin(),
                            data_.problems.end());
    if (data_.no_password_key) {
      summary.problems.push_back(
          "Its passwords couldn't be read: Compass wasn't given the key " +
          base::UTF16ToUTF8(BrowserName(job.profile.browser)) +
          " encrypts them with.");
    }

    // Firefox-based: history and passwords through Chromium's importer.
    uint16_t items = 0;
    if (ImportsHistoryThroughChromium(job.profile.browser)) {
      if (job.choice.history && CanImportHistory(job.profile.browser)) {
        items |= user_data_importer::HISTORY;
        summary.history = -1;
      }
      if (job.choice.passwords && CanImportPasswords(job.profile.browser)) {
        items |= user_data_importer::PASSWORDS;
        summary.passwords = -1;
      }
    }
    if (!items) {
      Finish(std::move(summary));
      return;
    }
    pending_ = std::move(summary);
    user_data_importer::SourceProfile source;
    source.importer_name = BrowserName(job.profile.browser);
    source.importer_type = user_data_importer::TYPE_FIREFOX;
    source.source_path = job.profile.path;
    source.app_path = job.profile.app_path;
    source.services_supported = items;
    source.locale = g_browser_process->GetApplicationLocale();
    source.profile = job.profile.name;
    // Deletes itself once done, after telling ImportEnded().
    host_ = new ExternalProcessImporterHost();
    host_->set_headless();
    host_->set_observer(this);
    host_->StartImportSettings(source, profile, items,
                               new ProfileWriter(profile));
  }

  // importer::ImporterProgressObserver:
  void ImportStarted() override {}
  void ImportItemStarted(user_data_importer::ImportItem item) override {}
  void ImportItemEnded(user_data_importer::ImportItem item) override {}
  void ImportEnded() override {
    host_ = nullptr;
    // May come from within StartImportSettings() (a locked profile).
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Importer::OnChromiumImportEnded,
                                  weak_factory_.GetWeakPtr()));
  }

  void OnChromiumImportEnded() {
    BrowserImportSummary summary = std::move(*pending_);
    pending_.reset();
    Finish(std::move(summary));
  }

  // Adds what |added| made in |profile| to |summary|, and lists |profile|'s
  // spaces in every window's switcher.
  void Summarize(const AddedSidebar& added,
                 Profile& profile,
                 BrowserImportSummary& summary) {
    for (SpaceModel* space : added.spaces) {
      summary.spaces.push_back(space->name());
    }
    if (!added.spaces.empty() && !result_.first_space.is_valid()) {
      result_.first_space = added.spaces.front()->id();
      result_.first_space_profile = profile.GetWeakPtr();
    }
    summary.pinned = added.pinned;
    summary.today = added.today;
    summary.favorites = added.favorites;
    if (!added.spaces.empty()) {
      if (SpaceDirectory* directory = SpaceDirectory::Get()) {
        directory->List(&profile);
      }
    }
  }

  // As a profile ------------------------------------------------------------

  void CopyWithEncryptor(scoped_refptr<os_crypt_async::Encryptor> encryptor) {
    const Job& job = jobs_[next_];
    ProfileManager* manager = g_browser_process->profile_manager();
    const base::FilePath to = manager->GenerateNextProfileDirectoryPath();
    ReadOptions options;
    options.bookmarks = job.choice.bookmarks;
    options.tabs = job.choice.tabs;
    CopyRunner()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&CopyAsProfile, job.profile, options, to,
                       manager->user_data_dir(), std::move(encryptor)),
        base::BindOnce(&Importer::OnCopied, weak_factory_.GetWeakPtr(), to));
  }

  void OnCopied(const base::FilePath& to, ProfileCopy copy) {
    const Job& job = jobs_[next_];
    BrowserImportSummary summary;
    summary.name = SpaceNameFor(job.profile);
    if (!copy.copy.ok) {
      summary.problems.push_back(copy.problem);
      Finish(std::move(summary));
      return;
    }
    summary.cookies_dropped = copy.copy.dropped_cookies + copy.copy.app_bound;
    data_ = std::move(copy.data);
    FinishReadingProfile(data_);
    g_browser_process->profile_manager()->CreateProfileAsync(
        to, base::BindOnce(&Importer::OnNewProfileLoaded,
                           weak_factory_.GetWeakPtr(), std::move(summary)));
  }

  void OnNewProfileLoaded(BrowserImportSummary summary, Profile* profile) {
    if (!profile) {
      summary.problems.push_back(
          "It was copied, but the new profile couldn't be opened.");
      Finish(std::move(summary));
      return;
    }
    if (ProfileAttributesEntry* entry =
            g_browser_process->profile_manager()
                ->GetProfileAttributesStorage()
                .GetProfileAttributesWithPath(profile->GetPath())) {
      entry->SetLocalProfileName(summary.name, /*is_default_name=*/false);
    }
    summary.profile = summary.name;
    SpaceService::Get(profile)->RunWhenLoaded(base::BindOnce(
        &Importer::OnNewProfileSpacesLoaded, weak_factory_.GetWeakPtr(),
        profile->GetWeakPtr(), std::move(summary)));
  }

  void OnNewProfileSpacesLoaded(base::WeakPtr<Profile> profile,
                                BrowserImportSummary summary) {
    if (!profile) {
      Finish(std::move(summary));
      return;
    }
    AddedSidebar added = AddImportedSidebar(
        SpaceService::Get(profile.get())->space_list(),
        ForNewProfile(std::move(data_.sidebar)), base::Time::Now(),
        /*into_new_profile=*/true);
    Summarize(added, *profile, summary);
    // They came with the copy, uncounted.
    summary.history = -1;
    summary.passwords = -1;
    summary.problems.insert(summary.problems.end(), data_.problems.begin(),
                            data_.problems.end());
    // On disk before the page asks what was imported.
    profile->GetPrefs()->CommitPendingWrite(base::BindOnce(
        &Importer::Finish, weak_factory_.GetWeakPtr(), std::move(summary)));
  }

  void Finish(BrowserImportSummary summary) {
    result_.summaries.push_back(std::move(summary));
    data_ = ProfileData();
    ++next_;
    Next();
  }

  base::WeakPtr<Profile> target_;
  std::vector<BrowserImportChoice> choices_;
  base::OnceCallback<void(BrowserImportResult)> callback_;
  scoped_refptr<base::SequencedTaskRunner> runner_;
  std::vector<Job> jobs_;
  size_t next_ = 0;
  ProfileData data_;
  // While Chromium's importer runs.
  std::optional<BrowserImportSummary> pending_;
  raw_ptr<ExternalProcessImporterHost> host_ = nullptr;
  BrowserImportResult result_;
  base::WeakPtrFactory<Importer> weak_factory_{this};
};

}  // namespace

ProfileOverview::ProfileOverview() = default;
ProfileOverview::ProfileOverview(const ProfileOverview&) = default;
ProfileOverview& ProfileOverview::operator=(const ProfileOverview&) = default;
ProfileOverview::~ProfileOverview() = default;

BrowserImportSummary::BrowserImportSummary() = default;
BrowserImportSummary::BrowserImportSummary(const BrowserImportSummary&) =
    default;
BrowserImportSummary& BrowserImportSummary::operator=(
    const BrowserImportSummary&) = default;
BrowserImportSummary::~BrowserImportSummary() = default;

BrowserImportResult::BrowserImportResult() = default;
BrowserImportResult::BrowserImportResult(BrowserImportResult&&) = default;
BrowserImportResult& BrowserImportResult::operator=(BrowserImportResult&&) =
    default;
BrowserImportResult::~BrowserImportResult() = default;

void GetBrowserImportOverview(
    Profile* target,
    base::OnceCallback<void(std::vector<ProfileOverview>)> callback) {
  ProfileManager* manager = g_browser_process->profile_manager();
  ReadRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&FindProfiles,
                     manager ? manager->user_data_dir() : base::FilePath()),
      base::BindOnce(&OnFound, target->GetWeakPtr(), std::move(callback)));
}

void ImportFromBrowsers(
    Profile* target,
    std::vector<BrowserImportChoice> choices,
    base::OnceCallback<void(BrowserImportResult)> callback) {
  (new Importer(target, std::move(choices), std::move(callback)))->Start();
}

}  // namespace compass::browser_import
