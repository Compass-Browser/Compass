// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/browser_importer.h"

#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/importer/profile_writer.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/browser_import/detect.h"
#include "compass/browser/browser_import/profile_reader.h"
#include "compass/browser/browser_import/space_import.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/history/core/browser/url_row.h"
#include "components/password_manager/core/browser/password_form.h"

namespace compass::browser_import {

namespace {

scoped_refptr<base::SequencedTaskRunner> ReadRunner() {
  return base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN});
}

// A profile found, with the sidebar it would bring.
struct Found {
  ProfileOverview overview;
  ImportedSidebar sidebar;
};

std::vector<Found> FindProfiles() {
  std::vector<Found> found;
  for (SourceProfile& profile : DetectProfiles()) {
    Found entry;
    entry.overview.running = IsRunning(profile);
    entry.overview.history = CanImportHistory(profile.browser);
    entry.overview.passwords = CanImportPasswords(profile.browser);
    // Only the sidebar: no Keychain prompt, no history yet.
    entry.sidebar = ReadProfile(profile, ReadOptions()).sidebar;
    for (const arc::Space& space : entry.sidebar.spaces) {
      entry.overview.spaces.push_back(space.name);
      entry.overview.bookmarks += CountTabs(space.pinned);
      entry.overview.tabs += CountTabs(space.today);
    }
    entry.overview.bookmarks += CountTabs(entry.sidebar.favorites);
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
          if (target) {
            const SpaceList& list =
                SpaceService::Get(target.get())->space_list();
            entry.overview.imported =
                !entry.sidebar.spaces.empty() &&
                std::ranges::all_of(entry.sidebar.spaces,
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

history::VisitSource VisitSourceFor(Browser browser) {
  // Chromium has no source for Chromium-based browsers; any "imported" one
  // keeps the visits apart from browsing.
  return history::SOURCE_IE_IMPORTED;
}

// Imports one profile after another, then deletes itself.
class Importer {
 public:
  Importer(Profile* target,
           std::vector<BrowserImportChoice> choices,
           base::OnceCallback<void(BrowserImportResult)> callback)
      : target_(target->GetWeakPtr()),
        choices_(std::move(choices)),
        callback_(std::move(callback)),
        runner_(ReadRunner()) {}

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
    for (SpaceModel* space : added.spaces) {
      summary.spaces.push_back(space->name());
    }
    if (!added.spaces.empty() && !result_.first_space.is_valid()) {
      result_.first_space = added.spaces.front()->id();
    }
    summary.pinned = added.pinned;
    summary.today = added.today;
    summary.favorites = added.favorites;
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
    if (data_.no_password_key) {
      summary.problems.push_back(
          "Its passwords couldn't be read: Compass wasn't given the key " +
          base::UTF16ToUTF8(BrowserName(job.profile.browser)) +
          " encrypts them with.");
    }
    Finish(std::move(summary));
  }

  void Finish(BrowserImportSummary summary) {
    if (target_ && !summary.spaces.empty()) {
      // Its spaces show in every window's switcher.
      if (SpaceDirectory* directory = SpaceDirectory::Get()) {
        directory->List(target_.get());
      }
    }
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
  ReadRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&FindProfiles),
      base::BindOnce(&OnFound, target->GetWeakPtr(), std::move(callback)));
}

void ImportFromBrowsers(
    Profile* target,
    std::vector<BrowserImportChoice> choices,
    base::OnceCallback<void(BrowserImportResult)> callback) {
  (new Importer(target, std::move(choices), std::move(callback)))->Start();
}

}  // namespace compass::browser_import
