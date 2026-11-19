// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/android/claude_tabs_android.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/functional/callback_helpers.h"
#include "base/i18n/case_conversion.h"
#include "base/i18n/time_formatting.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/send_tab_to_self_sync_service_factory.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "compass/browser/android/spaces_android.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "components/download/public/common/download_item.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/prefs/pref_service.h"
#include "components/send_tab_to_self/page_context.h"
#include "components/send_tab_to_self/send_tab_to_self_entry.h"
#include "components/send_tab_to_self/send_tab_to_self_model.h"
#include "components/send_tab_to_self/send_tab_to_self_sync_service.h"
#include "components/sync_device_info/remote_device_info.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace compass {

namespace {

using Result = claude::TabsDelegate::Result;

constexpr char kTabPrefix[] = "t";
constexpr char kNodePrefix[] = "n";
constexpr char kSpacePrefix[] = "s";
// Enough to tell a profile's entries apart; list_tabs and the tools check.
constexpr size_t kIdChars = 8;

constexpr char kNoPinnedInIncognito[] =
    "Incognito spaces have no Pinned section: nothing can be pinned there. "
    "Use today instead.";

std::string NodeId(const SidebarNode& node) {
  return kNodePrefix + node.id().AsLowercaseString().substr(0, kIdChars);
}

std::string SpaceId(const SpaceModel& space) {
  return kSpacePrefix + space.id().AsLowercaseString().substr(0, kIdChars);
}

std::string TabId(TabAndroid* tab) {
  return kTabPrefix + base::NumberToString(tab->GetAndroidId());
}

Result Text(std::string text) {
  Result result;
  result.result = claude::ToolResult::Text(std::move(text));
  return result;
}

Result Error(std::string text) {
  Result result;
  result.result = claude::ToolResult::Error(std::move(text));
  return result;
}

std::string Utf8(const std::u16string& text) {
  return base::UTF16ToUTF8(text);
}

const SidebarNode* RootOf(const SidebarNode* node) {
  while (node->parent()) {
    node = node->parent();
  }
  return node;
}

std::optional<size_t> OptionalIndex(const base::DictValue& input) {
  std::optional<int> index = input.FindInt("index");
  if (!index || *index < 0) {
    return std::nullopt;
  }
  return static_cast<size_t>(*index);
}

// The profile's regular tab with Android ID |tab_id|, in any window.
TabAndroid* FindTabById(Profile* profile, int tab_id) {
  for (TabModel* model : TabModelList::models()) {
    if (model->IsOffTheRecord() || model->GetProfile() != profile) {
      continue;
    }
    for (int i = 0; i < model->GetTabCount(); ++i) {
      TabAndroid* tab = model->GetTabAt(i);
      if (tab && tab->GetAndroidId() == tab_id) {
        return tab;
      }
    }
  }
  return nullptr;
}

// Whether |tab| is the one its window shows.
bool IsFront(TabAndroid* tab) {
  TabModel* model = TabModelList::GetTabModelForTabAndroid(tab);
  return model && model->GetActiveIndex() >= 0 &&
         model->GetTabAt(model->GetActiveIndex()) == tab;
}

void Close(TabAndroid* tab) {
  if (TabModel* model = TabModelList::GetTabModelForTabAndroid(tab)) {
    model->CloseTab(tab->GetHandle());
  }
}

}  // namespace

ClaudeTabsAndroid::ClaudeTabsAndroid(Profile* profile, Window* window)
    : profile_(profile), window_(window) {}

ClaudeTabsAndroid::~ClaudeTabsAndroid() = default;

bool ClaudeTabsAndroid::BrowserScopeAllowed() {
  return profile_->GetPrefs()->GetBoolean(claude::prefs::kBrowserScope);
}

SpacesAndroid* ClaudeTabsAndroid::spaces() {
  SpacesAndroid* spaces = SpacesAndroid::GetIfExists(profile_);
  return spaces && spaces->loaded() ? spaces : nullptr;
}

SidebarNode* ClaudeTabsAndroid::FindNode(const std::string& id,
                                         SpaceModel** space) {
  SpacesAndroid* all = spaces();
  if (!all || !id.starts_with(kNodePrefix)) {
    return nullptr;
  }
  SidebarNode* found = nullptr;
  SpaceModel* found_space = nullptr;
  auto visit = [&](auto& self, SpaceModel& model, SidebarNode* node) -> void {
    if (found || !node) {
      return;
    }
    if (node->type() != SidebarNode::Type::kRoot && NodeId(*node) == id) {
      found = node;
      found_space = &model;
      return;
    }
    for (const auto& child : node->children()) {
      self(self, model, child.get());
    }
  };
  auto search = [&](SpaceModel& model) {
    visit(visit, model, model.pinned_root());
    visit(visit, model, model.today_root());
  };
  search(all->list().favorites());
  for (const auto& model : all->list().spaces()) {
    search(*model);
  }
  if (space) {
    *space = found_space;
  }
  return found;
}

SpaceModel* ClaudeTabsAndroid::FindSpace(const std::string& name_or_id) {
  SpacesAndroid* all = spaces();
  if (!all) {
    return nullptr;
  }
  const std::u16string wanted =
      base::i18n::ToLower(base::UTF8ToUTF16(name_or_id));
  for (const auto& space : all->list().spaces()) {
    if (SpaceId(*space) == name_or_id ||
        base::i18n::ToLower(space->name()) == wanted) {
      return space.get();
    }
  }
  return nullptr;
}

SpaceModel* ClaudeTabsAndroid::ShownSpace() {
  SpacesAndroid* all = spaces();
  if (!all) {
    return nullptr;
  }
  return &all->SpaceOrActive(base::Uuid::ParseLowercase(window_->ShownSpace()));
}

TabAndroid* ClaudeTabsAndroid::TabOf(const SidebarNode& node) {
  SpacesAndroid* all = spaces();
  int tab_id = all ? all->TabFor(node.id()) : -1;
  return tab_id >= 0 ? FindTabById(profile_, tab_id) : nullptr;
}

std::string ClaudeTabsAndroid::ListTabs(const Context& context) {
  SpacesAndroid* all = spaces();
  if (!all) {
    return "The sidebar isn't loaded yet.";
  }
  SpaceModel* shown = ShownSpace();
  std::string text;
  auto tab_line = [&](const SidebarNode& node, int depth) {
    // A live tab's own title and URL: the entry's are saved lazily. The
    // user's name for it wins.
    TabAndroid* tab = TabOf(node);
    content::WebContents* contents = tab ? tab->GetContents() : nullptr;
    std::u16string title = node.custom_title();
    GURL url = node.url();
    if (tab) {
      url = tab->GetURL();
      if (title.empty()) {
        title = tab->GetTitle();
      }
    } else if (title.empty()) {
      title = node.title();
    }
    std::string line = std::string(depth * 2, ' ') + "- " + NodeId(node) +
                       ": " + Utf8(title) + " — " + url.spec();
    if (!contents) {
      line += " [unloaded]";
    } else {
      if (IsFront(tab)) {
        line += " [front]";
      }
      if (contents == context.target) {
        line += " [you act on this]";
      }
      if (contents->IsAudioMuted()) {
        line += " [muted]";
      } else if (contents->IsCurrentlyAudible()) {
        line += " [playing]";
      }
    }
    if (context.opened && context.opened->contains(NodeId(node))) {
      line += " [you opened it]";
    }
    text += line + "\n";
  };
  auto entries = [&](auto& self, const SidebarNode& parent,
                     int depth) -> void {
    for (const auto& child : parent.children()) {
      switch (child->type()) {
        case SidebarNode::Type::kTab:
          tab_line(*child, depth);
          break;
        case SidebarNode::Type::kSplit:
          text += std::string(depth * 2, ' ') + "- split " + NodeId(*child) +
                  ", side by side:\n";
          self(self, *child, depth + 1);
          break;
        case SidebarNode::Type::kFolder:
          text += std::string(depth * 2, ' ') + "- folder \"" +
                  Utf8(child->title()) + "\" (folder_id " + NodeId(*child) +
                  "):\n";
          self(self, *child, depth + 1);
          break;
        case SidebarNode::Type::kRoot:
          break;
      }
    }
  };

  SpaceModel& favorites = all->list().favorites();
  if (!favorites.pinned_root()->children().empty()) {
    text += "Favorites (every space):\n";
    entries(entries, *favorites.pinned_root(), 1);
  }
  for (const auto& space : all->list().spaces()) {
    text += base::StrCat({"\nSpace \"", Utf8(space->name()), "\" (",
                          SpaceId(*space), ")"});
    if (space.get() == shown) {
      text += " [shown on the user's screen]";
    }
    text += ":\n";
    if (space->has_pinned()) {
      text += "  Pinned:\n";
      entries(entries, *space->pinned_root(), 2);
    }
    if (space->today_root()) {
      text += "  Today:\n";
      entries(entries, *space->today_root(), 2);
    }
  }

  // Tabs with no entry: new tab pages, and the like.
  std::string others;
  for (TabModel* model : TabModelList::models()) {
    if (model->IsOffTheRecord() || model->GetProfile() != profile_) {
      continue;
    }
    for (int i = 0; i < model->GetTabCount(); ++i) {
      TabAndroid* tab = model->GetTabAt(i);
      if (!tab || all->NodeFor(tab->GetAndroidId()).is_valid()) {
        continue;
      }
      others += base::StrCat({"- ", TabId(tab), ": ", Utf8(tab->GetTitle()),
                              " — ", tab->GetURL().spec()});
      if (tab->GetContents() && tab->GetContents() == context.target) {
        others += " [you act on this]";
      }
      others += "\n";
    }
  }
  if (!others.empty()) {
    text += "\nOther tabs (not in the sidebar):\n" + others;
  }
  return text;
}

content::WebContents* ClaudeTabsAndroid::FindLiveTab(const std::string& id) {
  if (id.starts_with(kTabPrefix)) {
    int tab_id = 0;
    if (!base::StringToInt(id.substr(sizeof(kTabPrefix) - 1), &tab_id)) {
      return nullptr;
    }
    TabAndroid* tab = FindTabById(profile_, tab_id);
    return tab ? tab->GetContents() : nullptr;
  }
  SidebarNode* node = FindNode(id);
  TabAndroid* tab = node ? TabOf(*node) : nullptr;
  return tab ? tab->GetContents() : nullptr;
}

base::expected<content::WebContents*, std::string> ClaudeTabsAndroid::LoadTab(
    const std::string& id) {
  if (content::WebContents* live = FindLiveTab(id)) {
    return live;
  }
  SpaceModel* space = nullptr;
  SidebarNode* node = FindNode(id, &space);
  if (!node) {
    if (id.starts_with(kTabPrefix)) {
      return base::unexpected(base::StrCat(
          {"Tab ", id,
           " isn't loaded: Android loads it when it's shown (activate_tab)."}));
    }
    return base::unexpected(
        base::StrCat({"No tab ", id, ". list_tabs gives the ids."}));
  }
  if (node->type() != SidebarNode::Type::kTab) {
    return base::unexpected(base::StrCat(
        {id, " is a folder or a split: use one of its tabs."}));
  }
  // Favorites load in the user's window; a space's entries only while the
  // window shows their space.
  if (space != &spaces()->list().favorites() && space != ShownSpace()) {
    return base::unexpected(base::StrCat(
        {id, " isn't loaded, and the user's window shows another space than \"",
         Utf8(space->name()), "\": activate_tab shows it, or switch_space."}));
  }
  window_->LoadInBackground(node->id().AsLowercaseString(), node->url().spec());
  TabAndroid* tab = TabOf(*node);
  if (!tab || !tab->GetContents()) {
    return base::unexpected(base::StrCat({id, " couldn't be loaded."}));
  }
  return tab->GetContents();
}

content::WebContents* ClaudeTabsAndroid::OpenTab(const GURL& url,
                                                 bool active) {
  // The window on screen: a new tab there is a Today entry of its space.
  TabModel* model = nullptr;
  for (TabModel* candidate : TabModelList::models()) {
    if (candidate->IsOffTheRecord() || candidate->GetProfile() != profile_) {
      continue;
    }
    if (!model || candidate->IsActiveModel()) {
      model = candidate;
    }
  }
  if (!model) {
    return nullptr;
  }
  tabs::TabInterface* tab =
      model->OpenTab(url, model->GetTabCount(), /*foreground=*/active);
  return tab ? tab->GetContents() : nullptr;
}

std::string ClaudeTabsAndroid::IdOf(content::WebContents* contents) {
  TabAndroid* tab = contents ? TabAndroid::FromWebContents(contents) : nullptr;
  if (!tab) {
    return std::string();
  }
  SpacesAndroid* all = spaces();
  base::Uuid node_id =
      all ? all->NodeFor(tab->GetAndroidId()) : base::Uuid();
  SidebarNode* node = node_id.is_valid() ? all->FindNode(node_id) : nullptr;
  return node ? NodeId(*node) : TabId(tab);
}

void ClaudeTabsAndroid::RunTool(const std::string& name,
                                base::DictValue input,
                                const Context& context,
                                base::OnceCallback<void(Result)> done) {
  if (name == "search_history") {
    SearchHistory(input, std::move(done));
    return;
  }
  Result result;
  if (!spaces() && name != "list_downloads" && name != "send_tab_to_device") {
    result = Error("The sidebar isn't loaded yet; try again in a moment.");
  } else if (name == "close_tab") {
    result = CloseTab(input);
  } else if (name == "activate_tab") {
    result = ActivateTab(input);
  } else if (name == "move_tab") {
    result = MoveTab(input);
  } else if (name == "rename_tab") {
    result = RenameTab(input);
  } else if (name == "create_folder") {
    result = CreateFolder(input);
  } else if (name == "mute_tab") {
    result = MuteTab(input);
  } else if (name == "split_tabs") {
    result = SplitTabs(input);
  } else if (name == "switch_space") {
    result = SwitchSpace(input);
  } else if (name == "create_space") {
    result = CreateSpace(input);
  } else if (name == "list_downloads") {
    result = ListDownloads(input);
  } else if (name == "send_tab_to_device") {
    result = SendTabToDevice(input, context);
  } else {
    result = Error(base::StrCat({"Tool ", name, " isn't available here."}));
  }
  std::move(done).Run(std::move(result));
}

Result ClaudeTabsAndroid::CloseTab(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  if (!id) {
    return Error("Give a tab_id from list_tabs.");
  }
  const bool remove = input.FindBool("remove").value_or(false);
  SpaceModel* space = nullptr;
  SidebarNode* node = FindNode(*id, &space);
  if (!node) {
    // A tab outside the sidebar.
    content::WebContents* contents = FindLiveTab(*id);
    TabAndroid* tab = contents ? TabAndroid::FromWebContents(contents) : nullptr;
    if (!tab) {
      return Error(base::StrCat({"No tab ", *id, "."}));
    }
    Close(tab);
    return Text(base::StrCat({"Closed ", *id, "."}));
  }
  if (node->type() == SidebarNode::Type::kFolder) {
    return Error("That's a folder: close or move its tabs.");
  }
  // Its tabs: a tab's, or a split's panes'.
  std::vector<TabAndroid*> tabs;
  if (node->type() == SidebarNode::Type::kTab) {
    if (TabAndroid* tab = TabOf(*node)) {
      tabs.push_back(tab);
    }
  } else {
    for (const auto& pane : node->children()) {
      if (TabAndroid* tab = TabOf(*pane)) {
        tabs.push_back(tab);
      }
    }
  }
  const bool favorite = space == &spaces()->list().favorites();
  const bool pinned = RootOf(node) == space->pinned_root();
  if (remove) {
    // Out of the sidebar first, so its tabs close without an entry.
    space->Remove(node);
    for (TabAndroid* tab : tabs) {
      Close(tab);
    }
    return Text(base::StrCat({"Closed ", *id, " and removed it from ",
                              favorite ? "Favorites." : "the sidebar."}));
  }
  if (!pinned) {
    // A Today entry goes to its space's archive, as the user's close does
    // (SpacesAndroid::TabClosed()).
    if (tabs.empty()) {
      spaces()->ArchiveNode(node->id());
    }
    for (TabAndroid* tab : tabs) {
      Close(tab);
    }
    return Text(base::StrCat(
        {"Closed ", *id, "; it's in the space's archive."}));
  }
  // Pinned or a favorite: unloaded, and kept.
  for (TabAndroid* tab : tabs) {
    Close(tab);
  }
  return Text(!tabs.empty()
                  ? base::StrCat({"Closed ", *id,
                                  "; it stays in the sidebar, unloaded "
                                  "(remove: true takes it out)."})
                  : base::StrCat({*id,
                                  " wasn't loaded; remove: true takes it "
                                  "out of the sidebar."}));
}

Result ClaudeTabsAndroid::ActivateTab(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  if (!id) {
    return Error("Give a tab_id from list_tabs.");
  }
  SpaceModel* space = nullptr;
  SidebarNode* node = FindNode(*id, &space);
  TabAndroid* tab = nullptr;
  if (!node) {
    content::WebContents* contents = FindLiveTab(*id);
    tab = contents ? TabAndroid::FromWebContents(contents) : nullptr;
    TabModel* model = tab ? TabModelList::GetTabModelForTabAndroid(tab) : nullptr;
    if (!model) {
      return Error(base::StrCat({"No tab ", *id, "."}));
    }
    for (int i = 0; i < model->GetTabCount(); ++i) {
      if (model->GetTabAt(i) == tab) {
        model->SetActiveIndex(i);
      }
    }
  } else {
    if (node->type() == SidebarNode::Type::kSplit) {
      node = node->children().front().get();
    }
    if (node->type() != SidebarNode::Type::kTab) {
      return Error("That's a folder: activate one of its tabs.");
    }
    const bool favorite = space == &spaces()->list().favorites();
    window_->ShowNode(node->id().AsLowercaseString(), node->url().spec(),
                      favorite ? std::string()
                               : space->id().AsLowercaseString());
    tab = TabOf(*node);
  }
  if (!tab || !tab->GetContents()) {
    return Error(base::StrCat({*id, " couldn't be shown."}));
  }
  Result result = Text(base::StrCat({"Showing ", *id, " (\"",
                                     Utf8(tab->GetTitle()),
                                     "\") to the user. The page tools act on "
                                     "it."}));
  result.act_on = tab->GetContents()->GetWeakPtr();
  return result;
}

Result ClaudeTabsAndroid::MoveTab(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  const std::string* to = input.FindString("to");
  if (!id || !to) {
    return Error("Give tab_id and to.");
  }
  SpaceModel* from = nullptr;
  SidebarNode* node = FindNode(*id, &from);
  if (!node || node->type() == SidebarNode::Type::kFolder) {
    return Error(base::StrCat(
        {"No tab or split ", *id, " in the sidebar. list_tabs gives the ids."}));
  }
  SpaceModel& favorites = spaces()->list().favorites();
  SpaceModel* target = nullptr;
  SidebarNode* parent = nullptr;
  if (*to == "favorites") {
    target = &favorites;
    parent = favorites.pinned_root();
  } else if (*to == "folder") {
    const std::string* folder_id = input.FindString("folder_id");
    parent = folder_id ? FindNode(*folder_id, &target) : nullptr;
    if (!parent || parent->type() != SidebarNode::Type::kFolder) {
      return Error("Give the folder_id of a folder from list_tabs.");
    }
  } else if (*to == "pinned" || *to == "today") {
    if (const std::string* space = input.FindString("space")) {
      target = FindSpace(*space);
      if (!target) {
        return Error(base::StrCat({"No space \"", *space, "\"."}));
      }
    } else if (from == &favorites) {
      // Out of the favorites: into the space on screen.
      target = ShownSpace();
    } else {
      target = from;
    }
    if (!target) {
      return Error("Name a space.");
    }
    if (*to == "pinned" && !target->has_pinned()) {
      return Error(kNoPinnedInIncognito);
    }
    parent = *to == "pinned" ? target->pinned_root() : target->today_root();
  } else {
    return Error("to is pinned, today, favorites or folder.");
  }
  if (!parent) {
    return Error("That space has no such section.");
  }
  // Before the child now at |index| (the end without one).
  base::Uuid before;
  if (std::optional<size_t> index = OptionalIndex(input);
      index && *index < parent->children().size()) {
    before = parent->children()[*index]->id();
  }
  if (!spaces()->MoveNodeTo(node->id(), parent->id(), before)) {
    return Error("That move isn't possible (a folder can't go to Today, "
                 "say).");
  }
  return Text(base::StrCat({"Moved ", *id, "."}));
}

Result ClaudeTabsAndroid::RenameTab(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  const std::string* title = input.FindString("title");
  SpaceModel* space = nullptr;
  SidebarNode* node = id ? FindNode(*id, &space) : nullptr;
  if (!node || !title) {
    return Error("Give the tab_id of a sidebar entry and a title.");
  }
  std::u16string title16;
  base::TrimWhitespace(base::UTF8ToUTF16(*title), base::TRIM_ALL, &title16);
  if (node->type() == SidebarNode::Type::kFolder) {
    if (title16.empty()) {
      return Error("A folder needs a name.");
    }
    // The user asked for it, through Claude: their name, which Claude's
    // folder names don't replace (docs/design/ai-sidebar.md).
    space->SetFolderName(node, std::move(title16),
                         SidebarNode::NameSource::kUser);
  } else if (node->type() == SidebarNode::Type::kTab) {
    space->SetCustomTitle(node, std::move(title16));
  } else {
    return Error("Rename one of the split's tabs.");
  }
  return Text(base::StrCat({"Renamed ", *id, "."}));
}

Result ClaudeTabsAndroid::CreateFolder(const base::DictValue& input) {
  const std::string* name = input.FindString("name");
  if (!name || base::TrimWhitespaceASCII(*name, base::TRIM_ALL).empty()) {
    return Error("Give the folder a name.");
  }
  SpaceModel* space = nullptr;
  if (const std::string* space_name = input.FindString("space")) {
    space = FindSpace(*space_name);
    if (!space) {
      return Error(base::StrCat({"No space \"", *space_name, "\"."}));
    }
  } else {
    space = ShownSpace();
    if (!space) {
      return Error("Name a space.");
    }
  }
  const std::string* in = input.FindString("in");
  if (in && *in == "pinned" && !space->has_pinned()) {
    return Error(kNoPinnedInIncognito);
  }
  SidebarNode* parent =
      space->has_pinned() ? space->pinned_root() : space->today_root();
  if (in && *in == "today") {
    parent = space->today_root();
  } else if (in && *in != "pinned") {
    SpaceModel* folder_space = nullptr;
    parent = FindNode(*in, &folder_space);
    if (!parent || parent->type() != SidebarNode::Type::kFolder) {
      return Error("in is pinned, today, or a folder_id.");
    }
    space = folder_space;
  }
  if (!parent || !space->CanContain(parent, SidebarNode::Type::kFolder)) {
    return Error("The folder couldn't be made there.");
  }
  SidebarNode* folder = space->AddFolder(parent, parent->children().size(),
                                         base::UTF8ToUTF16(*name));
  if (!folder) {
    return Error("The folder couldn't be made there.");
  }
  return Text(base::StrCat({"Created folder \"", *name, "\": folder_id ",
                            NodeId(*folder), "."}));
}

Result ClaudeTabsAndroid::MuteTab(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  std::optional<bool> muted = input.FindBool("muted");
  if (!id || !muted) {
    return Error("Give tab_id and muted.");
  }
  content::WebContents* contents = FindLiveTab(*id);
  if (!contents) {
    SidebarNode* node = FindNode(*id);
    if (!node || node->type() != SidebarNode::Type::kTab) {
      return Error(base::StrCat({"No tab ", *id, "."}));
    }
    return Error(base::StrCat({*id,
                               " isn't loaded, so it plays nothing: "
                               "select_tab or activate_tab loads it."}));
  }
  contents->SetAudioMuted(*muted);
  return Text(base::StrCat({*muted ? "Muted " : "Unmuted ", *id, "."}));
}

Result ClaudeTabsAndroid::SplitTabs(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  const std::string* with = input.FindString("with_tab_id");
  SpaceModel* space = nullptr;
  SpaceModel* with_space = nullptr;
  SidebarNode* node = id ? FindNode(*id, &space) : nullptr;
  SidebarNode* target = with ? FindNode(*with, &with_space) : nullptr;
  if (!node || !target) {
    return Error("Give two tab_ids from list_tabs.");
  }
  if (space != with_space || !space->CanSplit(target, node)) {
    return Error("Those two can't be shown side by side (they must be tabs "
                 "of the same space, not favorites).");
  }
  space->Split(target, node);
  return Text(base::StrCat({*id, " and ", *with,
                            " are side by side now; activate_tab shows "
                            "them."}));
}

Result ClaudeTabsAndroid::SwitchSpace(const base::DictValue& input) {
  const std::string* name = input.FindString("space");
  SpaceModel* space = name ? FindSpace(*name) : nullptr;
  if (!space) {
    return Error("No such space. list_tabs lists them.");
  }
  window_->SwitchSpace(space->id().AsLowercaseString());
  if (ShownSpace() != space) {
    return Error("The window couldn't switch.");
  }
  return Text(base::StrCat(
      {"The user's window shows \"", Utf8(space->name()), "\" now."}));
}

Result ClaudeTabsAndroid::CreateSpace(const base::DictValue& input) {
  const std::string* name = input.FindString("name");
  if (!name || base::TrimWhitespaceASCII(*name, base::TRIM_ALL).empty()) {
    return Error("Give the space a name.");
  }
  SpaceList& list = spaces()->list();
  SpaceModel* space =
      list.AddSpace(list.spaces().size(), base::UTF8ToUTF16(*name));
  if (!space) {
    return Error("The space couldn't be made.");
  }
  std::string text = base::StrCat(
      {"Created space \"", *name, "\" (", SpaceId(*space), ")."});
  if (input.FindBool("show").value_or(false)) {
    window_->SwitchSpace(space->id().AsLowercaseString());
    if (ShownSpace() == space) {
      text += " The user's window shows it.";
    }
  }
  return Text(std::move(text));
}

void ClaudeTabsAndroid::SearchHistory(const base::DictValue& input,
                                      base::OnceCallback<void(Result)> done) {
  history::HistoryService* history = HistoryServiceFactory::GetForProfile(
      profile_, ServiceAccessType::EXPLICIT_ACCESS);
  if (!history) {
    std::move(done).Run(Error("History isn't available."));
    return;
  }
  history::QueryOptions options;
  options.max_count =
      std::clamp(input.FindInt("max_results").value_or(20), 1, 100);
  if (std::optional<int> days = input.FindInt("days"); days && *days > 0) {
    options.begin_time = base::Time::Now() - base::Days(*days);
  }
  options.duplicate_policy = history::QueryOptions::REMOVE_DUPLICATES_PER_DAY;
  const std::string* query = input.FindString("query");
  history->QueryHistory(
      base::UTF8ToUTF16(query ? *query : std::string()), options,
      base::BindOnce(
          [](base::OnceCallback<void(Result)> done,
             history::QueryResults results) {
            if (results.empty()) {
              std::move(done).Run(Text("No matching history."));
              return;
            }
            std::string text;
            for (const history::URLResult& row : results) {
              base::StrAppend(
                  &text,
                  {"- ", Utf8(row.title()), " — ", row.url().spec(), " (",
                   base::UTF16ToUTF8(base::TimeFormatShortDateAndTime(
                       row.visit_time())),
                   ")\n"});
            }
            std::move(done).Run(Text(std::move(text)));
          },
          std::move(done)),
      &history_tracker_);
}

Result ClaudeTabsAndroid::ListDownloads(const base::DictValue& input) {
  content::DownloadManager* manager = profile_->GetDownloadManager();
  if (!manager) {
    return Error("Downloads aren't available.");
  }
  std::vector<raw_ptr<download::DownloadItem, VectorExperimental>> items;
  manager->GetAllDownloads(&items);
  std::ranges::sort(items, [](const auto& a, const auto& b) {
    return a->GetStartTime() > b->GetStartTime();
  });
  const size_t limit = std::clamp(input.FindInt("limit").value_or(20), 1, 100);
  if (items.empty()) {
    return Text("No downloads.");
  }
  std::string text;
  for (size_t i = 0; i < items.size() && i < limit; ++i) {
    download::DownloadItem* item = items[i];
    const char* state = "in progress";
    switch (item->GetState()) {
      case download::DownloadItem::COMPLETE:
        state = "done";
        break;
      case download::DownloadItem::CANCELLED:
        state = "cancelled";
        break;
      case download::DownloadItem::INTERRUPTED:
        state = "failed";
        break;
      default:
        break;
    }
    base::StrAppend(
        &text,
        {"- ", item->GetFileNameToReportUser().AsUTF8Unsafe(), " (", state,
         ", ", base::NumberToString(item->GetReceivedBytes()), " of ",
         base::NumberToString(item->GetTotalBytes()), " bytes) from ",
         item->GetURL().spec(), ", saved at ",
         item->GetTargetFilePath().AsUTF8Unsafe(), ", ",
         base::UTF16ToUTF8(
             base::TimeFormatShortDateAndTime(item->GetStartTime())),
         "\n"});
  }
  return Text(std::move(text));
}

Result ClaudeTabsAndroid::SendTabToDevice(const base::DictValue& input,
                                          const Context& context) {
  send_tab_to_self::SendTabToSelfSyncService* service =
      SendTabToSelfSyncServiceFactory::GetForProfile(profile_);
  send_tab_to_self::SendTabToSelfModel* model =
      service ? service->GetSendTabToSelfModel() : nullptr;
  if (!model || !model->IsReady()) {
    return Error("Sending tabs needs Compass sync with another device.");
  }
  std::vector<syncer::RemoteDeviceInfo> devices =
      model->GetTargetDeviceInfoSortedList();
  const std::string* device = input.FindString("device");
  if (!device || devices.empty()) {
    if (devices.empty()) {
      return Text("No other devices sync with Compass.");
    }
    std::string text = "Devices:\n";
    for (const auto& info : devices) {
      text += "- " + info.device_name + "\n";
    }
    return Text(std::move(text));
  }
  auto target = std::ranges::find_if(devices, [&](const auto& info) {
    return base::EqualsCaseInsensitiveASCII(info.device_name, *device);
  });
  if (target == devices.end()) {
    return Error(base::StrCat({"No device \"", *device,
                               "\". Without device, the tool lists them."}));
  }
  // The tab: the one asked for, or the one Claude acts on.
  GURL url;
  std::string title;
  if (const std::string* id = input.FindString("tab_id")) {
    if (content::WebContents* contents = FindLiveTab(*id)) {
      url = contents->GetLastCommittedURL();
      title = Utf8(contents->GetTitle());
    } else if (SidebarNode* node = FindNode(*id);
               node && node->type() == SidebarNode::Type::kTab) {
      url = node->url();
      title = Utf8(node->display_title());
    }
  } else if (context.target) {
    url = context.target->GetLastCommittedURL();
    title = Utf8(context.target->GetTitle());
  }
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return Error("Only web pages can be sent; give a tab_id.");
  }
  model->SendEntry(url, title, target->cache_guid,
                   send_tab_to_self::PageContext(),
                   send_tab_to_self::NavigationHistory(), base::DoNothing(),
                   send_tab_to_self::ShareEntryPoint::kTabMenu);
  return Text(base::StrCat(
      {"Sent ", url.spec(), " to ", target->device_name, "."}));
}

}  // namespace compass
