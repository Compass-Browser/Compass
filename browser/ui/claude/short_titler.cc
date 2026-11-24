// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/short_titler.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/ai_provider.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/ui/claude/sidebar_spaces.h"
#include "components/prefs/pref_service.h"

namespace compass {

namespace {

// A title this long says enough.
constexpr size_t kMaxTitleLength = 200;

// |title| as one line, trimmed and shortened.
std::string LineTitle(const std::u16string& title16) {
  std::u16string line = title16;
  std::ranges::replace_if(
      line, [](char16_t c) { return c == u'\r' || c == u'\n' || c == u'\t'; },
      u' ');
  std::string title =
      base::UTF16ToUTF8(base::TrimWhitespace(line, base::TRIM_ALL));
  if (title.size() > kMaxTitleLength) {
    std::string cut;
    base::TruncateUTF8ToByteSize(title, kMaxTitleLength, &cut);
    title = cut + "\xe2\x80\xa6";
  }
  return title;
}

}  // namespace

ShortTitler::ShortTitler(PrefService* prefs,
                         claude::ClaudeQuick* quick,
                         GetLocale get_locale)
    : prefs_(prefs), quick_(quick), get_locale_(std::move(get_locale)) {
  enabled_ = IsEnabled();
  registrar_.Init(prefs_);
  const base::RepeatingClosure changed = base::BindRepeating(
      &ShortTitler::OnSettingsChanged, base::Unretained(this));
  registrar_.Add(claude::prefs::kShortTabTitles, changed);
  registrar_.Add(claude::prefs::kSidebarOnDevice, changed);
  claude::prefs::ObserveProviderPrefs(
      registrar_, {claude::prefs::kApiKey, claude::prefs::kBaseUrl}, changed);
}

ShortTitler::~ShortTitler() = default;

// static
ShortTitler* ShortTitler::Get(Profile* profile) {
  return ShortTitlerFactory::GetForProfile(profile);
}

// static
std::string ShortTitler::DescribeTitles(
    const std::vector<std::pair<std::u16string, std::string>>& tabs) {
  std::string text = "The tabs:\n";
  for (size_t i = 0; i < tabs.size(); ++i) {
    base::StrAppend(&text, {base::NumberToString(i + 1), ". ",
                            LineTitle(tabs[i].first)});
    if (!tabs[i].second.empty()) {
      base::StrAppend(&text, {" (", tabs[i].second, ")"});
    }
    text += "\n";
  }
  return text;
}

bool ShortTitler::IsEnabled() const {
  return quick_->IsEnabled(claude::prefs::kShortTabTitles);
}

// static
bool ShortTitler::IsEligible(const SpaceModel& space, const SidebarNode& tab) {
  return space.kind() == SpaceModel::Kind::kSpace &&
         tab.type() == SidebarNode::Type::kTab && tab.custom_title().empty() &&
         claude::NeedsShortTitle(tab.title());
}

// static
ShortTitler::Key ShortTitler::KeyOf(const SidebarNode& tab) {
  // The host only: never the path or the query.
  return {tab.url().is_valid() && tab.url().has_host()
              ? std::string(tab.url().host())
              : std::string(),
          tab.title()};
}

void ShortTitler::Observe(SpaceModel& space) {
  if (model_observations_.IsObservingSource(&space)) {
    return;
  }
  model_observations_.AddObservation(&space);
  spaces_.push_back(&space);
  ConsiderTree(space, *space.pinned_root());
  ConsiderTree(space, *space.today_root());
}

void ShortTitler::ObserveSpaces(SidebarSpaces* spaces) {
  space_service_ = spaces->GetWeakPtr();
  spaces->RunWhenLoaded(base::BindOnce(&ShortTitler::OnSpacesLoaded,
                                       weak_factory_.GetWeakPtr()));
}

void ShortTitler::OnSpacesLoaded() {
  if (!space_service_) {
    return;
  }
  SpaceList& list = space_service_->space_list();
  list_observation_.Observe(&list);
  // Favorites' tiles show no title: not followed.
  for (const auto& space : list.spaces()) {
    Observe(*space);
  }
}

void ShortTitler::Shutdown() {
  timer_.Stop();
  send_posted_ = false;
  pending_.clear();
  pending_order_.clear();
  model_observations_.RemoveAllObservations();
  spaces_.clear();
  list_observation_.Reset();
  registrar_.RemoveAll();
  weak_factory_.InvalidateWeakPtrs();
}

void ShortTitler::OnSpaceAdded(SpaceModel& space) {
  Observe(space);
}

void ShortTitler::OnSpaceWillBeRemoved(SpaceModel& space) {
  if (model_observations_.IsObservingSource(&space)) {
    model_observations_.RemoveObservation(&space);
  }
  std::erase(spaces_, &space);
  std::erase_if(pending_,
                [&](const auto& entry) { return entry.second == &space; });
}

void ShortTitler::OnNodeAdded(const SidebarNode& node) {
  if (SpaceModel* space = SpaceOf(node)) {
    ConsiderTree(*space, node);
  }
}

void ShortTitler::OnNodeChanged(const SidebarNode& node) {
  // A tab's title came or changed (SetTitle() dropped its short title), or
  // its custom title did.
  if (node.type() != SidebarNode::Type::kTab) {
    return;
  }
  if (SpaceModel* space = SpaceOf(node)) {
    Consider(*space, node);
  }
}

SpaceModel* ShortTitler::SpaceOf(const SidebarNode& node) {
  const SidebarNode* root = &node;
  while (root->parent()) {
    root = root->parent();
  }
  for (SpaceModel* space : spaces_) {
    if (space->pinned_root() == root || space->today_root() == root) {
      return space;
    }
  }
  return nullptr;
}

void ShortTitler::ConsiderTree(SpaceModel& space, const SidebarNode& node) {
  if (node.type() == SidebarNode::Type::kTab) {
    Consider(space, node);
    return;
  }
  for (const auto& child : node.children()) {
    ConsiderTree(space, *child);
  }
}

void ShortTitler::Consider(SpaceModel& space, const SidebarNode& tab) {
  if (!enabled_ || !IsEligible(space, tab)) {
    pending_.erase(tab.id());
    return;
  }
  const Key key = KeyOf(tab);
  if (auto cached = cache_.find(key); cached != cache_.end()) {
    pending_.erase(tab.id());
    if (!cached->second.empty() && tab.short_title() != cached->second) {
      // Not from inside the model's notification (this runs from
      // OnNodeChanged), whose observer list doesn't allow it.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&ShortTitler::ApplyCachedToTab,
                         weak_factory_.GetWeakPtr(), tab.id()));
    }
    return;
  }
  if (!tab.short_title().empty() || asking_.contains(key)) {
    // Its answer is on its way (and goes to every tab with that title).
    return;
  }
  if (pending_.insert_or_assign(tab.id(), &space).second) {
    pending_order_.push_back(tab.id());
  }
  if (pending_.size() >= kMaxBatch) {
    SendSoon();
    return;
  }
  // Titles that changed in the next kBatchDelay go with this one.
  if (!timer_.IsRunning() && !send_posted_) {
    timer_.Start(FROM_HERE, batch_delay_,
                 base::BindOnce(&ShortTitler::SendBatch,
                                base::Unretained(this)));
  }
}

void ShortTitler::ApplyCachedToTab(const base::Uuid& tab_id) {
  // Found again: its space may have gone while the task waited.
  for (SpaceModel* space : spaces_) {
    SidebarNode* tab = space->FindById(tab_id);
    if (!tab) {
      continue;
    }
    if (!enabled_ || !IsEligible(*space, *tab)) {
      return;
    }
    auto cached = cache_.find(KeyOf(*tab));
    if (cached != cache_.end() && !cached->second.empty() &&
        tab->short_title() != cached->second) {
      space->SetShortTitle(tab, cached->second);
    }
    return;
  }
}

void ShortTitler::ConsiderAll() {
  for (SpaceModel* space : spaces_) {
    ConsiderTree(*space, *space->pinned_root());
    ConsiderTree(*space, *space->today_root());
  }
}

void ShortTitler::SendSoon() {
  timer_.Stop();
  if (send_posted_) {
    return;
  }
  send_posted_ = true;
  // Not from inside the model's notification.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&ShortTitler::SendBatch, weak_factory_.GetWeakPtr()));
}

void ShortTitler::SendBatch() {
  timer_.Stop();
  send_posted_ = false;
  std::vector<Key> keys;
  std::vector<std::pair<std::u16string, std::string>> tabs;
  size_t taken = 0;
  for (const base::Uuid& id : pending_order_) {
    if (keys.size() == kMaxBatch) {
      break;
    }
    ++taken;
    auto it = pending_.find(id);
    if (it == pending_.end()) {
      continue;
    }
    SpaceModel* space = it->second;
    pending_.erase(it);
    const SidebarNode* tab = space->FindById(id);
    // Closed, renamed or given a short title meanwhile.
    if (!tab || !enabled_ || !IsEligible(*space, *tab)) {
      continue;
    }
    const Key key = KeyOf(*tab);
    if (cache_.contains(key) || asking_.contains(key) ||
        std::ranges::contains(keys, key)) {
      continue;
    }
    keys.push_back(key);
    tabs.emplace_back(key.second, key.first);
  }
  pending_order_.erase(pending_order_.begin(),
                       pending_order_.begin() + taken);
  if (pending_.empty()) {
    pending_order_.clear();
  } else {
    if (pending_.size() >= kMaxBatch) {
      SendSoon();
    } else {
      timer_.Start(FROM_HERE, batch_delay_,
                   base::BindOnce(&ShortTitler::SendBatch,
                                  base::Unretained(this)));
    }
  }
  if (keys.empty()) {
    return;
  }
  asking_.insert(keys.begin(), keys.end());
  claude::QuickRequest request;
  request.feature_pref = claude::prefs::kShortTabTitles;
  request.model = prefs_->GetString(
      claude::prefs::CurrentPref(*prefs_, claude::prefs::kShortTabTitlesModel));
  request.system = claude::SidebarPrompt("short_titles", get_locale_.Run());
  claude::SetOnDevicePrompt(request, "short_titles", get_locale_.Run());
  request.user = DescribeTitles(tabs);
  request.max_tokens = claude::ClaudeQuick::kMaxTokens;
  // Automatic: the hourly limit and the back-off apply.
  request.background = true;
  quick_->Ask(std::move(request),
              base::BindOnce(&ShortTitler::OnReply, weak_factory_.GetWeakPtr(),
                             std::move(keys)));
}

void ShortTitler::OnReply(std::vector<Key> keys,
                          claude::ClaudeQuick::Result result) {
  for (const Key& key : keys) {
    asking_.erase(key);
  }
  if (!result.has_value()) {
    // Off meanwhile, or no answer (the hourly limit, the back-off, an
    // error): those titles stay long until they change.
    return;
  }
  std::vector<std::string> ids;
  for (size_t i = 0; i < keys.size(); ++i) {
    ids.push_back(base::NumberToString(i + 1));
  }
  const std::map<std::string, std::u16string> titles =
      claude::ParseShortTitlesReply(*result, ids);
  if (cache_.size() + keys.size() > kMaxCache) {
    cache_.clear();
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    auto title = titles.find(ids[i]);
    // A short title that isn't shorter is none.
    std::u16string short_title;
    if (title != titles.end() && title->second.size() < keys[i].second.size() &&
        title->second != keys[i].second) {
      short_title = title->second;
    }
    cache_[keys[i]] = std::move(short_title);
  }
  if (!enabled_) {
    return;
  }
  for (const Key& key : keys) {
    ApplyCached(key);
  }
}

void ShortTitler::ApplyCached(const Key& key) {
  auto cached = cache_.find(key);
  if (cached == cache_.end() || cached->second.empty()) {
    return;
  }
  for (SpaceModel* space : spaces_) {
    std::vector<base::Uuid> matches;
    auto collect = [&](auto& self, const SidebarNode& node) -> void {
      if (node.type() == SidebarNode::Type::kTab) {
        if (IsEligible(*space, node) && KeyOf(node) == key) {
          matches.push_back(node.id());
        }
        return;
      }
      for (const auto& child : node.children()) {
        self(self, *child);
      }
    };
    collect(collect, *space->pinned_root());
    collect(collect, *space->today_root());
    for (const base::Uuid& id : matches) {
      if (SidebarNode* tab = space->FindById(id)) {
        space->SetShortTitle(tab, cached->second);
      }
    }
  }
}

void ShortTitler::OnSettingsChanged() {
  const bool enabled = IsEnabled();
  if (enabled == enabled_) {
    return;
  }
  enabled_ = enabled;
  if (enabled_) {
    ConsiderAll();
    return;
  }
  // Off: every short title goes, and nothing more is asked.
  timer_.Stop();
  pending_.clear();
  pending_order_.clear();
  for (SpaceModel* space : spaces_) {
    std::vector<base::Uuid> ids;
    auto collect = [&](auto& self, const SidebarNode& node) -> void {
      if (node.type() == SidebarNode::Type::kTab) {
        if (!node.short_title().empty()) {
          ids.push_back(node.id());
        }
        return;
      }
      for (const auto& child : node.children()) {
        self(self, *child);
      }
    };
    collect(collect, *space->pinned_root());
    collect(collect, *space->today_root());
    for (const base::Uuid& id : ids) {
      if (SidebarNode* tab = space->FindById(id)) {
        space->SetShortTitle(tab, std::u16string());
      }
    }
  }
}

// static
ShortTitlerFactory* ShortTitlerFactory::GetInstance() {
  static base::NoDestructor<ShortTitlerFactory> instance;
  return instance.get();
}

// static
ShortTitler* ShortTitlerFactory::GetForProfile(Profile* profile) {
  return static_cast<ShortTitler*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

ShortTitlerFactory::ShortTitlerFactory()
    : ProfileKeyedServiceFactory(
          "CompassShortTitler",
          // Incognito never asks: its own profile has none.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {
  DependsOn(claude::ClaudeQuickFactory::GetInstance());
}

ShortTitlerFactory::~ShortTitlerFactory() = default;

std::unique_ptr<KeyedService>
ShortTitlerFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  claude::ClaudeQuick* quick = claude::ClaudeQuick::Get(profile);
  if (!quick) {
    return nullptr;
  }
  auto titler = std::make_unique<ShortTitler>(
      profile->GetPrefs(), quick, base::BindRepeating([] {
        return g_browser_process->GetApplicationLocale();
      }));
  // Not while the profile is being made: its spaces read the sidebar.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<ShortTitler> titler, Profile* profile) {
                       if (!titler) {
                         return;
                       }
                       if (SidebarSpaces* spaces = SidebarSpaces::Get(profile)) {
                         titler->ObserveSpaces(spaces);
                       }
                     },
                     titler->GetWeakPtr(), base::Unretained(profile)));
  return titler;
}

}  // namespace compass
