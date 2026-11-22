// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/adblock_service.h"

#include <algorithm>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/clock.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "compass/browser/adblock/filter_list_updater.h"
#include "components/prefs/pref_service.h"
#include "components/subresource_filter/content/browser/ruleset_service.h"
#include "components/subresource_filter/core/browser/ruleset_version.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace compass::adblock {

namespace {

AdBlockService* g_service = nullptr;

std::vector<std::string> EnabledIds(const PrefService* local_state) {
  std::vector<std::string> ids;
  for (const FilterList& list : GetEnabledFilterLists(local_state)) {
    ids.push_back(list.id);
  }
  return ids;
}

// The folder of Google's Safe Browsing ruleset, which Compass doesn't use
// (patches/chrome-browser-component_updater-registration.cc.patch).
void DeleteSafeBrowsingRuleset(const base::FilePath& user_data_dir) {
  base::DeletePathRecursively(
      user_data_dir.Append(FILE_PATH_LITERAL("Subresource Filter")));
}

// The list files no list has any more (a list removed while it
// downloaded).
void DeleteOrphanedLists(const base::FilePath& user_data_dir,
                         std::vector<std::string> ids) {
  base::FilePath dir = GetListTextPath(user_data_dir, "x").DirName();
  base::FileEnumerator files(dir, /*recursive=*/false,
                             base::FileEnumerator::FILES);
  for (base::FilePath file = files.Next(); !file.empty();
       file = files.Next()) {
    std::string id = file.BaseName().RemoveExtension().MaybeAsASCII();
    if (!std::ranges::contains(ids, id)) {
      base::DeleteFile(file);
    }
  }
}

}  // namespace

AdBlockService::ListInfo::ListInfo() = default;
AdBlockService::ListInfo::ListInfo(const ListInfo&) = default;
AdBlockService::ListInfo& AdBlockService::ListInfo::operator=(
    const ListInfo&) = default;
AdBlockService::ListInfo::~ListInfo() = default;

AdBlockService::AdBlockService(
    PrefService* local_state,
    const base::FilePath& user_data_dir,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    subresource_filter::RulesetService* ruleset_service,
    const base::Clock* clock)
    : local_state_(local_state),
      user_data_dir_(user_data_dir),
      ruleset_service_(ruleset_service),
      clock_(clock),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {
  updater_ = std::make_unique<FilterListUpdater>(
      local_state_, user_data_dir_, std::move(url_loader_factory),
      file_task_runner_, clock_,
      base::BindRepeating(&AdBlockService::OnListUpdated,
                          weak_factory_.GetWeakPtr()));
  pref_change_registrar_.Init(local_state_);
  pref_change_registrar_.Add(
      prefs::kEnabled, base::BindRepeating(&AdBlockService::OnEnabledPrefChanged,
                                           base::Unretained(this)));
  for (const char* pref : {prefs::kCustomLists, prefs::kDisabledLists}) {
    pref_change_registrar_.Add(
        pref, base::BindRepeating(&AdBlockService::OnListsPrefChanged,
                                  base::Unretained(this)));
  }
  pref_change_registrar_.Add(
      prefs::kListState,
      base::BindRepeating(&AdBlockService::NotifyChanged,
                          base::Unretained(this)));
  CHECK(!g_service);
  g_service = this;
}

AdBlockService::~AdBlockService() {
  CHECK_EQ(g_service, this);
  g_service = nullptr;
}

// static
AdBlockService* AdBlockService::Get() {
  return g_service;
}

void AdBlockService::Start() {
  file_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&DeleteSafeBrowsingRuleset, user_data_dir_));
  file_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&DeleteOrphanedLists, user_data_dir_,
                     [this] {
                       std::vector<std::string> ids;
                       for (const FilterList& list :
                            GetFilterLists(local_state_)) {
                         ids.push_back(list.id);
                       }
                       return ids;
                     }()));
  // Supplied at every launch: the same lists give the same version, which
  // the service skips, and a new Chromium's index format indexes again.
  Rebuild();
  first_update_timer_.Start(FROM_HERE, kFirstUpdateDelay, this,
                            &AdBlockService::MaybeUpdateLists);
  update_timer_.Start(FROM_HERE, kUpdateInterval, this,
                      &AdBlockService::MaybeUpdateLists);
}

bool AdBlockService::IsEnabled() const {
  return local_state_->GetBoolean(prefs::kEnabled);
}

void AdBlockService::SetEnabled(bool enabled) {
  local_state_->SetBoolean(prefs::kEnabled, enabled);
}

std::vector<AdBlockService::ListInfo> AdBlockService::GetLists() const {
  std::vector<ListInfo> infos;
  for (FilterList& list : GetFilterLists(local_state_)) {
    ListInfo& info = infos.emplace_back();
    info.state = GetFilterListState(local_state_, list.id);
    info.updating = updater_->current() == list.id;
    info.list = std::move(list);
  }
  return infos;
}

void AdBlockService::SetListEnabled(std::string_view id, bool enabled) {
  SetFilterListEnabled(local_state_, id, enabled);
  // A list turned on that was never downloaded, or long ago.
  if (enabled && IsEnabled()) {
    updater_->Update(/*force=*/false, base::OnceClosure());
  }
}

std::string AdBlockService::AddList(std::string_view url) {
  std::string error = ValidateCustomListUrl(local_state_, url);
  if (!error.empty()) {
    return error;
  }
  std::string id = AddCustomFilterList(
      local_state_, GURL(base::TrimWhitespaceASCII(url, base::TRIM_ALL)));
  if (IsEnabled()) {
    updater_->UpdateList(id);
  }
  NotifyChanged();
  return std::string();
}

void AdBlockService::RemoveList(std::string_view id) {
  std::optional<FilterList> list = FindFilterList(local_state_, id);
  if (!list || list->builtin) {
    return;
  }
  RemoveCustomFilterList(local_state_, id);
  file_task_runner_->PostTask(
      FROM_HERE,
      base::GetDeleteFileCallback(GetListTextPath(user_data_dir_, id)));
}

void AdBlockService::UpdateNow() {
  updater_->Update(/*force=*/true, base::OnceClosure());
  NotifyChanged();
}

bool AdBlockService::IsUpdating() const {
  return updater_->is_updating();
}

void AdBlockService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void AdBlockService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void AdBlockService::SetListTextForTesting(const std::string& id,
                                           const std::string& text,
                                           base::OnceClosure published) {
  published_for_testing_ = std::move(published);
  file_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath path, std::string text) {
            base::CreateDirectory(path.DirName());
            base::ImportantFileWriter::WriteFileAtomically(path, text);
          },
          GetListTextPath(user_data_dir_, id), text),
      base::BindOnce(&AdBlockService::Rebuild, weak_factory_.GetWeakPtr()));
}

void AdBlockService::OnListsPrefChanged() {
  ScheduleRebuild();
  NotifyChanged();
}

void AdBlockService::OnEnabledPrefChanged() {
  // The rules stay published when the blocker is off: nothing activates
  // them (page_activation_throttle.cc), and turning it on again is instant.
  if (IsEnabled()) {
    MaybeUpdateLists();
  }
  NotifyChanged();
}

void AdBlockService::ScheduleRebuild() {
  rebuild_timer_.Start(FROM_HERE, kRebuildDelay, this,
                       &AdBlockService::Rebuild);
}

void AdBlockService::Rebuild() {
  rebuild_timer_.Stop();
  if (building_) {
    rebuild_again_ = true;
    return;
  }
  building_ = true;
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&BuildRulesetFile, user_data_dir_,
                     EnabledIds(local_state_)),
      base::BindOnce(&AdBlockService::OnRulesetBuilt,
                     weak_factory_.GetWeakPtr()));
}

void AdBlockService::OnRulesetBuilt(std::optional<BuiltRuleset> built) {
  building_ = false;
  if (built) {
    if (built->counts) {
      for (const auto& [id, counts] : *built->counts) {
        FilterListState state = GetFilterListState(local_state_, id);
        state.url_rules = counts.url_rules;
        state.css_rules = counts.css_rules;
        state.skipped = counts.skipped;
        SetFilterListState(local_state_, id, state);
      }
    }
    if (ruleset_service_) {
      subresource_filter::UnindexedRulesetInfo info;
      info.content_version = built->content_version;
      info.ruleset_path = built->path;
      if (published_for_testing_) {
        subresource_filter::IndexedRulesetVersion version(
            ruleset_service_->config().filter_tag);
        version.ReadFromPrefs(local_state_);
        if (version.IsCurrentFormatVersion() &&
            version.content_version == info.content_version) {
          std::move(published_for_testing_).Run();
        } else {
          ruleset_service_->SetRulesetPublishedCallbackForTesting(
              std::move(published_for_testing_));
        }
      }
      ruleset_service_->IndexAndStoreAndPublishRulesetIfNeeded(info);
    }
  }
  if (std::exchange(rebuild_again_, false)) {
    Rebuild();
  }
}

void AdBlockService::OnListUpdated(const std::string& id, bool changed) {
  if (changed) {
    ScheduleRebuild();
  }
  NotifyChanged();
}

void AdBlockService::MaybeUpdateLists() {
  if (IsEnabled()) {
    updater_->Update(/*force=*/false, base::OnceClosure());
    NotifyChanged();
  }
}

void AdBlockService::NotifyChanged() {
  for (Observer& observer : observers_) {
    observer.OnFilterListsChanged();
  }
}

}  // namespace compass::adblock
