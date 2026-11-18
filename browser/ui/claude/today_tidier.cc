// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/today_tidier.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/ui/claude/sidebar_spaces.h"
#include "components/prefs/pref_service.h"

namespace compass {

namespace {

// A title this long says enough.
constexpr size_t kMaxTitleLength = 100;

// |tab|'s title as one line, trimmed and shortened.
std::string LineTitle(const SidebarNode& tab) {
  std::u16string title16 = tab.display_title();
  std::ranges::replace_if(
      title16,
      [](char16_t c) { return c == u'\r' || c == u'\n' || c == u'\t'; },
      u' ');
  std::string title =
      base::UTF16ToUTF8(base::TrimWhitespace(title16, base::TRIM_ALL));
  if (title.size() > kMaxTitleLength) {
    std::string cut;
    base::TruncateUTF8ToByteSize(title, kMaxTitleLength, &cut);
    title = cut + "\xe2\x80\xa6";
  }
  return title;
}

// |tab|'s host, if its page has one: never its path or query.
std::string Host(const SidebarNode& tab) {
  return tab.url().is_valid() && tab.url().has_host()
             ? std::string(tab.url().host())
             : std::string();
}

}  // namespace

TodayTidier::TodayTidier(Profile* profile, claude::ClaudeQuick* quick)
    : profile_(profile), quick_(quick) {}

TodayTidier::~TodayTidier() = default;

// static
TodayTidier* TodayTidier::Get(Profile* profile) {
  return TodayTidierFactory::GetForProfile(profile);
}

// static
std::vector<const SidebarNode*> TodayTidier::LooseEntries(
    const SpaceModel& space) {
  std::vector<const SidebarNode*> entries;
  for (const auto& entry : space.today_root()->children()) {
    if (entry->type() == SidebarNode::Type::kTab ||
        entry->type() == SidebarNode::Type::kSplit) {
      entries.push_back(entry.get());
    }
  }
  return entries;
}

// static
std::string TodayTidier::DescribeEntries(
    const std::vector<const SidebarNode*>& entries) {
  std::string text = "Today's loose entries:\n";
  for (size_t i = 0; i < entries.size(); ++i) {
    const SidebarNode& entry = *entries[i];
    std::vector<const SidebarNode*> tabs;
    if (entry.type() == SidebarNode::Type::kSplit) {
      for (const auto& pane : entry.children()) {
        tabs.push_back(pane.get());
      }
    } else {
      tabs.push_back(&entry);
    }
    std::vector<std::string> titles;
    std::vector<std::string> hosts;
    for (const SidebarNode* tab : tabs) {
      std::string title = LineTitle(*tab);
      std::string host = Host(*tab);
      if (title.empty()) {
        title = host;
      }
      if (!title.empty()) {
        titles.push_back(std::move(title));
      }
      if (!host.empty() && !std::ranges::contains(hosts, host)) {
        hosts.push_back(std::move(host));
      }
    }
    base::StrAppend(&text, {base::NumberToString(i + 1), ". ",
                            titles.empty() ? std::string("(untitled)")
                                           : base::JoinString(titles, " + ")});
    if (!hosts.empty()) {
      base::StrAppend(&text, {" (", base::JoinString(hosts, ", "), ")"});
    }
    text += "\n";
  }
  return text;
}

bool TodayTidier::IsEnabled() const {
  return quick_->IsEnabled(claude::prefs::kTidyToday);
}

bool TodayTidier::CanTidy(const SpaceModel& space) const {
  return IsEnabled() && space.kind() == SpaceModel::Kind::kSpace &&
         !IsBusy(space.id()) && LooseEntries(space).size() >= kMinEntries;
}

bool TodayTidier::IsBusy(const base::Uuid& space_id) const {
  return busy_.contains(space_id);
}

void TodayTidier::Tidy(SpaceModel& space) {
  if (!CanTidy(space)) {
    return;
  }
  std::vector<const SidebarNode*> entries = LooseEntries(space);
  if (entries.size() > kMaxEntries) {
    entries.resize(kMaxEntries);
  }
  std::vector<base::Uuid> ids;
  for (const SidebarNode* entry : entries) {
    ids.push_back(entry->id());
  }
  claude::QuickRequest request;
  request.feature_pref = claude::prefs::kTidyToday;
  request.model =
      profile_->GetPrefs()->GetString(claude::prefs::kTidyTodayModel);
  request.system = claude::SidebarPrompt(
      "tidy_today", g_browser_process->GetApplicationLocale());
  request.user = DescribeEntries(entries);
  request.max_tokens = claude::ClaudeQuick::kMaxTokens;
  // The user asked: no hourly limit, no waiting out a back-off.
  request.background = false;
  const base::Uuid space_id = space.id();
  SetBusy(space_id, true);
  quick_->Ask(std::move(request),
              base::BindOnce(&TodayTidier::OnReply, weak_factory_.GetWeakPtr(),
                             space_id, std::move(ids)));
}

void TodayTidier::OnReply(const base::Uuid& space_id,
                          std::vector<base::Uuid> ids,
                          claude::ClaudeQuick::Result result) {
  SetBusy(space_id, false);
  SpaceModel* space = FindSpace(space_id);
  if (!space) {
    return;
  }
  if (!result.has_value()) {
    switch (result.error()) {
      case claude::QuickError::kOff:
        // Turned off meanwhile: nothing to say.
        return;
      case claude::QuickError::kNotSetUp:
        Fail(space_id, "Couldn't tidy Today: Claude isn't set up.");
        return;
      case claude::QuickError::kTimeout:
        Fail(space_id, "Couldn't tidy Today: Claude took too long.");
        return;
      case claude::QuickError::kFailed:
      case claude::QuickError::kRateLimited:
      case claude::QuickError::kBackingOff:
        Fail(space_id, "Couldn't tidy Today: Claude couldn't be reached.");
        return;
      case claude::QuickError::kRefused:
      case claude::QuickError::kBadReply:
        Fail(space_id, "Couldn't tidy Today.");
        return;
    }
    return;
  }
  // Turned off while Claude thought: nothing changes.
  if (!IsEnabled()) {
    return;
  }
  Apply(*space, ids, *result);
}

void TodayTidier::Apply(SpaceModel& space,
                        const std::vector<base::Uuid>& ids,
                        const base::DictValue& reply) {
  // The request's ids are 1 to n, in the listed order.
  std::vector<std::string> names;
  for (size_t i = 0; i < ids.size(); ++i) {
    names.push_back(base::NumberToString(i + 1));
  }
  std::vector<SpaceModel::TidyGroup> groups;
  for (claude::TidyFolder& folder : claude::ParseTidyReply(reply, names)) {
    std::vector<base::Uuid> entries;
    for (const std::string& name : folder.entries) {
      size_t index = 0;
      if (base::StringToSizeT(name, &index) && index >= 1 &&
          index <= ids.size()) {
        entries.push_back(ids[index - 1]);
      }
    }
    groups.emplace_back(std::move(folder.name), std::move(entries));
  }
  // Entries closed or moved meanwhile are skipped (TidyToday()).
  SpaceModel::TodayTidy tidy = space.TidyToday(std::move(groups));
  const base::Uuid space_id = space.id();
  if (tidy.folders.empty()) {
    Fail(space_id, "Claude found nothing to group.");
    return;
  }
  const size_t folders = tidy.folders.size();
  const int undo_id = next_undo_id_++;
  undos_.emplace(undo_id, Undoable{space_id, std::move(tidy)});
  while (undos_.size() > kMaxUndos) {
    undos_.erase(undos_.begin());
  }
  for (Observer& observer : observers_) {
    observer.OnTidied(space_id, undo_id, folders);
  }
}

bool TodayTidier::Undo(int undo_id) {
  auto it = undos_.find(undo_id);
  if (it == undos_.end()) {
    return false;
  }
  Undoable undoable = std::move(it->second);
  undos_.erase(it);
  SpaceModel* space = FindSpace(undoable.space_id);
  return space && space->UndoTidy(undoable.tidy);
}

void TodayTidier::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void TodayTidier::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void TodayTidier::Shutdown() {
  undos_.clear();
  busy_.clear();
  weak_factory_.InvalidateWeakPtrs();
}

SpaceModel* TodayTidier::FindSpace(const base::Uuid& space_id) {
  SidebarSpaces* spaces = SidebarSpaces::Get(profile_);
  return spaces ? spaces->space_list().FindSpace(space_id) : nullptr;
}

void TodayTidier::SetBusy(const base::Uuid& space_id, bool busy) {
  if (busy ? !busy_.insert(space_id).second : busy_.erase(space_id) == 0) {
    return;
  }
  for (Observer& observer : observers_) {
    observer.OnTidyBusyChanged(space_id);
  }
}

void TodayTidier::Fail(const base::Uuid& space_id, const std::string& message) {
  for (Observer& observer : observers_) {
    observer.OnTidyFailed(space_id, message);
  }
}

// static
TodayTidierFactory* TodayTidierFactory::GetInstance() {
  static base::NoDestructor<TodayTidierFactory> instance;
  return instance.get();
}

// static
TodayTidier* TodayTidierFactory::GetForProfile(Profile* profile) {
  return static_cast<TodayTidier*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

TodayTidierFactory::TodayTidierFactory()
    : ProfileKeyedServiceFactory(
          "CompassTodayTidier",
          // Incognito never asks: its own profile has none.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {
  DependsOn(claude::ClaudeQuickFactory::GetInstance());
}

TodayTidierFactory::~TodayTidierFactory() = default;

std::unique_ptr<KeyedService>
TodayTidierFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  claude::ClaudeQuick* quick = claude::ClaudeQuick::Get(profile);
  if (!quick) {
    return nullptr;
  }
  return std::make_unique<TodayTidier>(profile, quick);
}

}  // namespace compass
