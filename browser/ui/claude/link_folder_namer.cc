// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/link_folder_namer.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
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
constexpr size_t kMaxTitleLength = 120;

// Every tab in |node|'s subtree, in order.
void CollectTabs(const SidebarNode& node,
                 std::vector<const SidebarNode*>& tabs) {
  for (const auto& child : node.children()) {
    if (child->type() == SidebarNode::Type::kTab) {
      tabs.push_back(child.get());
    } else {
      CollectTabs(*child, tabs);
    }
  }
}

// The folder |node| is in (|node| itself if it's one), through splits.
const SidebarNode* FolderOf(const SidebarNode* node) {
  while (node && node->type() != SidebarNode::Type::kFolder) {
    if (node->type() == SidebarNode::Type::kRoot) {
      return nullptr;
    }
    node = node->parent();
  }
  return node;
}

}  // namespace

LinkFolderNamer::LinkFolderNamer(Profile* profile, claude::ClaudeQuick* quick)
    : profile_(profile), quick_(quick) {
  // Not while the profile is being made: its spaces read the sidebar.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<LinkFolderNamer> namer) {
                       if (!namer) {
                         return;
                       }
                       SidebarSpaces* spaces = SidebarSpaces::Get(namer->profile_);
                       if (!spaces) {
                         return;
                       }
                       namer->space_service_ = spaces->GetWeakPtr();
                       spaces->RunWhenLoaded(base::BindOnce(
                           &LinkFolderNamer::OnSpacesLoaded, namer));
                     },
                     weak_factory_.GetWeakPtr()));
}

LinkFolderNamer::~LinkFolderNamer() = default;

// static
LinkFolderNamer* LinkFolderNamer::Get(Profile* profile) {
  return LinkFolderNamerFactory::GetForProfile(profile);
}

// static
std::string LinkFolderNamer::DescribeFolder(const SidebarNode& folder) {
  std::vector<const SidebarNode*> tabs;
  CollectTabs(folder, tabs);
  std::string text = "The folder's tabs:\n";
  for (size_t i = 0; i < tabs.size() && i < kMaxTabs; ++i) {
    const SidebarNode& tab = *tabs[i];
    // Titles and hosts only: never paths, queries or what pages hold.
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
    const std::string host = tab.url().is_valid() && tab.url().has_host()
                                 ? std::string(tab.url().host())
                                 : std::string();
    if (title.empty() && host.empty()) {
      continue;
    }
    base::StrAppend(&text, {"- ", title.empty() ? host : title});
    if (!host.empty() && !title.empty()) {
      base::StrAppend(&text, {" (", host, ")"});
    }
    text += "\n";
  }
  return text;
}

bool LinkFolderNamer::IsEnabled() const {
  return quick_->IsEnabled(claude::prefs::kNameLinkFolders);
}

void LinkFolderNamer::Suggest(const base::Uuid& folder_id) {
  Ask(folder_id, /*background=*/false);
}

void LinkFolderNamer::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void LinkFolderNamer::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void LinkFolderNamer::Shutdown() {
  waiting_.clear();
  model_observations_.RemoveAllObservations();
  list_observation_.Reset();
}

void LinkFolderNamer::OnSpacesLoaded() {
  if (!space_service_) {
    return;
  }
  SpaceList& list = space_service_->space_list();
  list_observation_.Observe(&list);
  for (const auto& space : list.spaces()) {
    model_observations_.AddObservation(space.get());
  }
}

void LinkFolderNamer::OnSpaceAdded(SpaceModel& space) {
  model_observations_.AddObservation(&space);
}

void LinkFolderNamer::OnSpaceWillBeRemoved(SpaceModel& space) {
  model_observations_.RemoveObservation(&space);
}

void LinkFolderNamer::OnNodeAdded(const SidebarNode& node) {
  OnFolderChanged(node.parent());
}

void LinkFolderNamer::OnNodeMoved(const SidebarNode& node,
                                  const SidebarNode& old_parent,
                                  size_t old_index) {
  OnFolderChanged(node.parent());
}

void LinkFolderNamer::OnNodeChanged(const SidebarNode& node) {
  // A tab's title came (a link's page loaded), or the folder changed (the
  // user renamed it).
  OnFolderChanged(&node);
}

void LinkFolderNamer::OnNodeWillBeRemoved(const SidebarNode& node) {
  if (node.type() == SidebarNode::Type::kFolder) {
    asked_at_.erase(node.id());
    waiting_.erase(node.id());
  }
}

SidebarNode* LinkFolderNamer::FindFolder(const base::Uuid& id,
                                         SpaceModel** space_out) {
  if (!space_service_) {
    return nullptr;
  }
  for (const auto& space : space_service_->space_list().spaces()) {
    if (SidebarNode* node = space->FindById(id);
        node && node->type() == SidebarNode::Type::kFolder) {
      if (space_out) {
        *space_out = space.get();
      }
      return node;
    }
  }
  return nullptr;
}

bool LinkFolderNamer::IsEligible(const SidebarNode& folder) const {
  if (folder.type() != SidebarNode::Type::kFolder ||
      !folder.link_folder_here() || folder.is_live_folder() ||
      folder.name_source() == SidebarNode::NameSource::kUser ||
      !space_service_ || !IsEnabled()) {
    return false;
  }
  for (const auto& space : space_service_->space_list().spaces()) {
    if (space->today_root()->Contains(&folder)) {
      return true;
    }
  }
  return false;
}

void LinkFolderNamer::OnFolderChanged(const SidebarNode* node) {
  const SidebarNode* folder = FolderOf(node);
  if (!folder) {
    return;
  }
  const base::Uuid id = folder->id();
  if (!IsEligible(*folder)) {
    waiting_.erase(id);
    return;
  }
  size_t threshold = 0;
  for (size_t at : kThresholds) {
    if (folder->children().size() >= at) {
      threshold = at;
    }
  }
  auto waiting = waiting_.find(id);
  if (threshold <= asked_at_[id] && waiting == waiting_.end()) {
    return;
  }
  if (waiting == waiting_.end()) {
    waiting = waiting_.emplace(id, std::make_unique<base::OneShotTimer>())
                  .first;
  }
  // 5 s after its last change.
  waiting->second->Start(FROM_HERE, debounce_,
                         base::BindOnce(&LinkFolderNamer::OnWaited,
                                        base::Unretained(this), id));
}

void LinkFolderNamer::OnWaited(const base::Uuid& folder_id) {
  waiting_.erase(folder_id);
  SidebarNode* folder = FindFolder(folder_id, nullptr);
  if (!folder || !IsEligible(*folder)) {
    return;
  }
  size_t threshold = 0;
  for (size_t at : kThresholds) {
    if (folder->children().size() >= at) {
      threshold = at;
    }
  }
  if (threshold <= asked_at_[folder_id]) {
    return;
  }
  asked_at_[folder_id] = threshold;
  Ask(folder_id, /*background=*/true);
}

void LinkFolderNamer::Ask(const base::Uuid& folder_id, bool background) {
  SidebarNode* folder = FindFolder(folder_id, nullptr);
  if (!folder || asking_.contains(folder_id)) {
    return;
  }
  claude::QuickRequest request;
  request.feature_pref = claude::prefs::kNameLinkFolders;
  const PrefService& prefs = *profile_->GetPrefs();
  request.model = prefs.GetString(
      claude::prefs::CurrentPref(prefs, claude::prefs::kNameLinkFoldersModel));
  request.system = claude::SidebarPrompt(
      "folder_name", g_browser_process->GetApplicationLocale());
  request.user = DescribeFolder(*folder);
  request.max_tokens = 60;
  request.background = background;
  asking_.emplace(folder_id, background);
  quick_->Ask(std::move(request),
              base::BindOnce(&LinkFolderNamer::OnName,
                             weak_factory_.GetWeakPtr(), folder_id));
}

void LinkFolderNamer::OnName(const base::Uuid& folder_id,
                             claude::ClaudeQuick::Result result) {
  auto asking = asking_.find(folder_id);
  const bool background = asking == asking_.end() || asking->second;
  if (asking != asking_.end()) {
    asking_.erase(asking);
  }
  if (!result.has_value()) {
    return;
  }
  SpaceModel* space = nullptr;
  SidebarNode* folder = FindFolder(folder_id, &space);
  const std::string* text = result->FindString("name");
  if (!folder || !text) {
    return;
  }
  // A name the user gave while Claude thought wins, and so does turning the
  // switch off; Suggest a Name asks for one whatever the name is.
  if (background && !IsEligible(*folder)) {
    return;
  }
  std::optional<std::u16string> name = claude::SanitizeAiName(*text);
  if (!name || *name == folder->title()) {
    return;
  }
  space->SetFolderName(folder, std::move(*name),
                       SidebarNode::NameSource::kClaude);
  for (Observer& observer : observers_) {
    observer.OnFolderNamed(folder_id);
  }
}

// static
LinkFolderNamerFactory* LinkFolderNamerFactory::GetInstance() {
  static base::NoDestructor<LinkFolderNamerFactory> instance;
  return instance.get();
}

// static
LinkFolderNamer* LinkFolderNamerFactory::GetForProfile(Profile* profile) {
  return static_cast<LinkFolderNamer*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

LinkFolderNamerFactory::LinkFolderNamerFactory()
    : ProfileKeyedServiceFactory(
          "CompassLinkFolderNamer",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {
  DependsOn(claude::ClaudeQuickFactory::GetInstance());
}

LinkFolderNamerFactory::~LinkFolderNamerFactory() = default;

std::unique_ptr<KeyedService>
LinkFolderNamerFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  claude::ClaudeQuick* quick = claude::ClaudeQuick::Get(profile);
  if (!quick) {
    return nullptr;
  }
  return std::make_unique<LinkFolderNamer>(profile, quick);
}

}  // namespace compass
