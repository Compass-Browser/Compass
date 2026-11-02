// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/claude_tabs.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/functional/callback_helpers.h"
#include "base/i18n/case_conversion.h"
#include "base/i18n/time_formatting.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/send_tab_to_self_sync_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/live_tab_registry.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/download/public/common/download_item.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/prefs/pref_service.h"
#include "components/send_tab_to_self/page_context.h"
#include "components/send_tab_to_self/send_tab_to_self_entry.h"
#include "components/send_tab_to_self/send_tab_to_self_model.h"
#include "components/send_tab_to_self/send_tab_to_self_sync_service.h"
#include "components/send_tab_to_self/target_device_info.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"

namespace compass::claude {

namespace {

using Result = TabsDelegate::Result;

constexpr char kTabPrefix[] = "t";
constexpr char kNodePrefix[] = "n";
constexpr char kSpacePrefix[] = "s";
// Enough to tell a profile's entries apart; list_tabs and the tools check.
constexpr size_t kIdChars = 8;

std::string NodeId(const SidebarNode& node) {
  return kNodePrefix + node.id().AsLowercaseString().substr(0, kIdChars);
}

std::string SpaceId(const SpaceModel& space) {
  return kSpacePrefix + space.id().AsLowercaseString().substr(0, kIdChars);
}

std::string TabId(const tabs::TabInterface* tab) {
  return kTabPrefix + base::NumberToString(tab->GetHandle().raw_value());
}

Result Text(std::string text) {
  Result result;
  result.result = ToolResult::Text(std::move(text));
  return result;
}

Result Error(std::string text) {
  Result result;
  result.result = ToolResult::Error(std::move(text));
  return result;
}

std::string Utf8(const std::u16string& text) {
  return base::UTF16ToUTF8(text);
}

// The window normal tabs live in, not a hidden one.
bool IsShownWindow(BrowserWindowInterface* browser) {
  if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    return false;
  }
  WindowFeatures* features = WindowFeatures::From(browser);
  return !features ||
         (!features->is_parking_window() && !features->swapped_away());
}

// The root (Pinned, Today) or favorites' root a node is under.
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

}  // namespace

ClaudeTabs::ClaudeTabs(Profile* profile, BrowserWindowInterface* home)
    : profile_(profile), home_(home) {}

ClaudeTabs::~ClaudeTabs() = default;

bool ClaudeTabs::BrowserScopeAllowed() {
  return profile_->GetPrefs()->GetBoolean(prefs::kBrowserScope);
}

BrowserWindowInterface* ClaudeTabs::Home() {
  BrowserWindowInterface* home = nullptr;
  BrowserWindowInterface* last_active = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile_ || !IsShownWindow(browser)) {
          return true;
        }
        if (!last_active) {
          last_active = browser;
        }
        if (browser == home_) {
          home = browser;
          return false;
        }
        return true;
      });
  return home ? home : last_active;
}

BrowserWindowInterface* ClaudeTabs::WindowShowing(const SpaceModel& space) {
  // The chat's window first.
  BrowserWindowInterface* home = Home();
  if (home) {
    WindowFeatures* features = WindowFeatures::From(home);
    if (features && &features->space_model() == &space) {
      return home;
    }
  }
  BrowserWindowInterface* found = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile_ || !IsShownWindow(browser)) {
          return true;
        }
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features && &features->space_model() == &space) {
          found = browser;
          return false;
        }
        return true;
      });
  return found;
}

SidebarNode* ClaudeTabs::FindNode(const std::string& id, SpaceModel** space) {
  SpaceService* service = SpaceService::Get(profile_);
  if (!service || !id.starts_with(kNodePrefix)) {
    return nullptr;
  }
  SidebarNode* found = nullptr;
  SpaceModel* found_space = nullptr;
  auto visit = [&](auto& self, SpaceModel& model, SidebarNode* node) -> void {
    if (found) {
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
  search(service->space_list().favorites());
  for (const auto& model : service->space_list().spaces()) {
    search(*model);
  }
  if (space) {
    *space = found_space;
  }
  return found;
}

SpaceModel* ClaudeTabs::FindSpace(const std::string& name_or_id) {
  SpaceService* service = SpaceService::Get(profile_);
  if (!service) {
    return nullptr;
  }
  const std::u16string wanted =
      base::i18n::ToLower(base::UTF8ToUTF16(name_or_id));
  for (const auto& space : service->space_list().spaces()) {
    if (SpaceId(*space) == name_or_id ||
        base::i18n::ToLower(space->name()) == wanted) {
      return space.get();
    }
  }
  return nullptr;
}

std::string ClaudeTabs::ListTabs(const Context& context) {
  SpaceService* service = SpaceService::Get(profile_);
  if (!service) {
    return "No tabs.";
  }
  LiveTabRegistry& registry = service->registry();
  BrowserWindowInterface* home = Home();
  WindowFeatures* home_features = home ? WindowFeatures::From(home) : nullptr;

  std::string text;
  auto tab_line = [&](const SidebarNode& node, int depth) {
    // A live tab's own title and URL: the entry's are saved lazily. The
    // user's name for it wins.
    tabs::TabInterface* tab = registry.GetTab(&node);
    std::u16string title = node.custom_title();
    GURL url = node.url();
    if (tab) {
      url = tab->GetContents()->GetVisibleURL();
      if (title.empty()) {
        title = tab->GetContents()->GetTitle();
      }
    } else if (title.empty()) {
      title = node.title();
    }
    std::string line = std::string(depth * 2, ' ') + "- " + NodeId(node) +
                       ": " + Utf8(title) + " — " + url.spec();
    if (!tab) {
      line += " [unloaded]";
    } else {
      content::WebContents* contents = tab->GetContents();
      BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
      if (browser && IsShownWindow(browser) &&
          browser->GetTabStripModel()->GetActiveTab() == tab) {
        line += " [front]";
      }
      if (contents == context.target) {
        line += " [you act on this]";
      }
      switch (registry.GetAudioState(&node)) {
        case LiveTabRegistry::AudioState::kMuted:
          line += " [muted]";
          break;
        case LiveTabRegistry::AudioState::kAudible:
          line += " [playing]";
          break;
        case LiveTabRegistry::AudioState::kNone:
          break;
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

  SpaceModel& favorites = service->space_list().favorites();
  if (!favorites.pinned_root()->children().empty()) {
    text += "Favorites (every space):\n";
    entries(entries, *favorites.pinned_root(), 1);
  }
  for (const auto& space : service->space_list().spaces()) {
    text += base::StrCat({"\nSpace \"", Utf8(space->name()), "\" (",
                          SpaceId(*space), ")"});
    if (home_features && &home_features->space_model() == space.get()) {
      text += " [shown in the user's window]";
    } else if (WindowShowing(*space)) {
      text += " [shown in another window]";
    }
    text += ":\n  Pinned:\n";
    entries(entries, *space->pinned_root(), 2);
    text += "  Today:\n";
    entries(entries, *space->today_root(), 2);
  }

  // Tabs outside the sidebar: popups, app windows.
  std::string others;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile_) {
          return true;
        }
        TabStripModel* model = browser->GetTabStripModel();
        for (int i = 0; i < model->count(); ++i) {
          tabs::TabInterface* tab = model->GetTabAtIndex(i);
          if (registry.GetNode(tab)) {
            continue;
          }
          content::WebContents* contents = tab->GetContents();
          others += base::StrCat({"- ", TabId(tab), ": ",
                                  Utf8(contents->GetTitle()), " — ",
                                  contents->GetVisibleURL().spec()});
          if (contents == context.target) {
            others += " [you act on this]";
          }
          others += "\n";
        }
        return true;
      });
  if (!others.empty()) {
    text += "\nOther windows (not in the sidebar):\n" + others;
  }
  return text;
}

content::WebContents* ClaudeTabs::FindLiveTab(const std::string& id) {
  if (id.starts_with(kTabPrefix)) {
    int raw = 0;
    if (!base::StringToInt(id.substr(sizeof(kTabPrefix) - 1), &raw)) {
      return nullptr;
    }
    tabs::TabInterface* tab = tabs::TabHandle(raw).Get();
    if (!tab || !tab->GetContents() ||
        Profile::FromBrowserContext(tab->GetContents()->GetBrowserContext()) !=
            profile_) {
      return nullptr;
    }
    return tab->GetContents();
  }
  SidebarNode* node = FindNode(id);
  SpaceService* service = SpaceService::Get(profile_);
  if (!node || !service) {
    return nullptr;
  }
  tabs::TabInterface* tab = service->registry().GetTab(node);
  return tab ? tab->GetContents() : nullptr;
}

base::expected<content::WebContents*, std::string> ClaudeTabs::LoadTab(
    const std::string& id) {
  if (content::WebContents* live = FindLiveTab(id)) {
    return live;
  }
  SpaceModel* space = nullptr;
  SidebarNode* node = FindNode(id, &space);
  if (!node) {
    return base::unexpected(
        base::StrCat({"No tab ", id, ". list_tabs gives the ids."}));
  }
  if (node->type() != SidebarNode::Type::kTab) {
    return base::unexpected(base::StrCat(
        {id, " is a folder or a split: use one of its tabs."}));
  }
  // Favorites load in the user's window; a space's entries in a window
  // showing it.
  SpaceService* service = SpaceService::Get(profile_);
  BrowserWindowInterface* window =
      space == &service->space_list().favorites() ? Home()
                                                  : WindowShowing(*space);
  WindowFeatures* features = window ? WindowFeatures::From(window) : nullptr;
  if (!features || !features->has_tab_strip_bridge()) {
    return base::unexpected(base::StrCat(
        {id, " isn't loaded, and no window shows its space \"",
         Utf8(space->name()), "\": activate_tab shows it, or switch_space."}));
  }
  tabs::TabInterface* tab =
      features->tab_strip_bridge().OpenNodeInBackground(node);
  if (!tab) {
    return base::unexpected(base::StrCat({id, " couldn't be loaded."}));
  }
  return tab->GetContents();
}

content::WebContents* ClaudeTabs::OpenTab(const GURL& url, bool active) {
  BrowserWindowInterface* home = Home();
  if (!home) {
    return nullptr;
  }
  // A new tab is a new Today entry of the window's space (TabStripBridge).
  NavigateParams params(home, url, ui::PAGE_TRANSITION_LINK);
  params.disposition = active ? WindowOpenDisposition::NEW_FOREGROUND_TAB
                              : WindowOpenDisposition::NEW_BACKGROUND_TAB;
  Navigate(&params);
  return params.navigated_or_inserted_contents;
}

std::string ClaudeTabs::IdOf(content::WebContents* contents) {
  const tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab) {
    return std::string();
  }
  SpaceService* service = SpaceService::Get(profile_);
  const SidebarNode* node = service ? service->registry().GetNode(tab) : nullptr;
  return node ? NodeId(*node) : TabId(tab);
}

void ClaudeTabs::RunTool(const std::string& name,
                         base::DictValue input,
                         const Context& context,
                         base::OnceCallback<void(Result)> done) {
  if (name == "search_history") {
    SearchHistory(input, std::move(done));
    return;
  }
  Result result;
  if (name == "close_tab") {
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

Result ClaudeTabs::CloseTab(const base::DictValue& input) {
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
    tabs::TabInterface* tab =
        contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
    if (!tab) {
      return Error(base::StrCat({"No tab ", *id, "."}));
    }
    tab->Close();
    return Text(base::StrCat({"Closed ", *id, "."}));
  }
  if (node->type() == SidebarNode::Type::kFolder) {
    return Error("That's a folder: close or move its tabs.");
  }
  SpaceService* service = SpaceService::Get(profile_);
  const bool kept = RootOf(node) == space->pinned_root() && !remove;
  if (!kept) {
    // Removing the entry closes its live tabs (TabStripBridge).
    space->Remove(node);
    return Text(base::StrCat(
        {"Closed ", *id, space == &service->space_list().favorites()
                             ? " and removed it from Favorites."
                             : "."}));
  }
  // Pinned or a favorite: unloaded, and kept.
  std::vector<SidebarNode*> tab_nodes;
  if (node->type() == SidebarNode::Type::kTab) {
    tab_nodes.push_back(node);
  } else {
    for (const auto& pane : node->children()) {
      tab_nodes.push_back(pane.get());
    }
  }
  bool closed = false;
  for (SidebarNode* tab_node : tab_nodes) {
    if (tabs::TabInterface* tab = service->registry().GetTab(tab_node)) {
      tab->Close();
      closed = true;
    }
  }
  return Text(closed ? base::StrCat({"Closed ", *id,
                                     "; it stays in the sidebar, unloaded "
                                     "(remove: true takes it out)."})
                     : base::StrCat({*id,
                                     " wasn't loaded; remove: true takes it "
                                     "out of the sidebar."}));
}

Result ClaudeTabs::ActivateTab(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  if (!id) {
    return Error("Give a tab_id from list_tabs.");
  }
  SpaceModel* space = nullptr;
  SidebarNode* node = FindNode(*id, &space);
  BrowserWindowInterface* home = Home();
  tabs::TabInterface* tab = nullptr;
  if (!node) {
    content::WebContents* contents = FindLiveTab(*id);
    tab = contents ? tabs::TabInterface::MaybeGetFromContents(contents)
                   : nullptr;
    if (!tab) {
      return Error(base::StrCat({"No tab ", *id, "."}));
    }
    BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
    TabStripModel* model = browser->GetTabStripModel();
    model->ActivateTabAt(model->GetIndexOfTab(tab));
  } else {
    if (node->type() == SidebarNode::Type::kSplit) {
      node = node->children().front().get();
    }
    if (node->type() != SidebarNode::Type::kTab) {
      return Error("That's a folder: activate one of its tabs.");
    }
    if (!home) {
      return Error("No window to show it in.");
    }
    SpaceService* service = SpaceService::Get(profile_);
    BrowserWindowInterface* window = home;
    if (space != &service->space_list().favorites()) {
      window = WindowShowing(*space);
      if (!window) {
        window = SwitchWindowToSpace(home, profile_, *space);
      }
    }
    WindowFeatures* features = window ? WindowFeatures::From(window) : nullptr;
    if (!features || !features->has_tab_strip_bridge()) {
      return Error("The tab's space couldn't be shown.");
    }
    tab = features->tab_strip_bridge().OpenNode(node);
    if (!tab) {
      return Error(base::StrCat({*id, " couldn't be opened."}));
    }
    TabStripModel* model = window->GetTabStripModel();
    model->ActivateTabAt(model->GetIndexOfTab(tab));
  }
  Result result = Text(base::StrCat(
      {"Showing ", *id, " (\"", Utf8(tab->GetContents()->GetTitle()),
       "\") to the user. The page tools act on it."}));
  result.act_on = tab->GetContents()->GetWeakPtr();
  return result;
}

Result ClaudeTabs::MoveTab(const base::DictValue& input) {
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
  SpaceService* service = SpaceService::Get(profile_);
  SpaceModel& favorites = service->space_list().favorites();
  // Where to: the favorites, or a space's section or folder.
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
      // Out of the favorites: into the user's window's space.
      BrowserWindowInterface* home = Home();
      WindowFeatures* features = home ? WindowFeatures::From(home) : nullptr;
      if (!features) {
        return Error("No window to take it to.");
      }
      target = &features->space_model();
    } else {
      target = from;
    }
    parent = *to == "pinned" ? target->pinned_root() : target->today_root();
  } else {
    return Error("to is pinned, today, favorites or folder.");
  }

  size_t index = OptionalIndex(input).value_or(parent->children().size());
  index = std::min(index, parent->children().size());
  if (target != from) {
    // Another space, or into or out of the favorites.
    MoveEntry(profile_, node, *target, parent, index);
  } else {
    if (!from->CanMove(node, parent)) {
      return Error("That move isn't possible (a folder can't go to Today, "
                   "say).");
    }
    // Move() counts the index after the node has left its parent.
    if (node->parent() == parent && parent->GetIndexOf(node) < index) {
      --index;
    }
    from->Move(node, parent, std::min(index, parent->children().size()));
  }
  SpaceModel* now = nullptr;
  if (!FindNode(*id, &now) || now != target || !parent->Contains(node)) {
    return Error("The sidebar refused that move.");
  }
  return Text(base::StrCat({"Moved ", *id, "."}));
}

Result ClaudeTabs::RenameTab(const base::DictValue& input) {
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
    space->SetTitle(node, std::move(title16));
  } else if (node->type() == SidebarNode::Type::kTab) {
    space->SetCustomTitle(node, std::move(title16));
  } else {
    return Error("Rename one of the split's tabs.");
  }
  return Text(base::StrCat({"Renamed ", *id, "."}));
}

Result ClaudeTabs::CreateFolder(const base::DictValue& input) {
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
    BrowserWindowInterface* home = Home();
    WindowFeatures* features = home ? WindowFeatures::From(home) : nullptr;
    if (!features) {
      return Error("No window: name a space.");
    }
    space = &features->space_model();
  }
  const std::string* in = input.FindString("in");
  SidebarNode* parent = space->pinned_root();
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
  SidebarNode* folder = space->AddFolder(parent, parent->children().size(),
                                         base::UTF8ToUTF16(*name));
  if (!folder) {
    return Error("The folder couldn't be made there.");
  }
  return Text(base::StrCat({"Created folder \"", *name, "\": folder_id ",
                            NodeId(*folder), "."}));
}

Result ClaudeTabs::MuteTab(const base::DictValue& input) {
  const std::string* id = input.FindString("tab_id");
  std::optional<bool> muted = input.FindBool("muted");
  if (!id || !muted) {
    return Error("Give tab_id and muted.");
  }
  if (content::WebContents* contents = FindLiveTab(*id)) {
    SetTabAudioMuted(contents, *muted, TabMutedReason::kAudioIndicator,
                     std::string());
  } else {
    SpaceModel* space = nullptr;
    SidebarNode* node = FindNode(*id, &space);
    if (!node || node->type() != SidebarNode::Type::kTab) {
      return Error(base::StrCat({"No tab ", *id, "."}));
    }
    // Unloaded: it opens muted (or not).
    space->SetMuted(node, *muted);
  }
  return Text(base::StrCat({*muted ? "Muted " : "Unmuted ", *id, "."}));
}

Result ClaudeTabs::SplitTabs(const base::DictValue& input) {
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

Result ClaudeTabs::SwitchSpace(const base::DictValue& input) {
  const std::string* name = input.FindString("space");
  SpaceModel* space = name ? FindSpace(*name) : nullptr;
  if (!space) {
    return Error("No such space. list_tabs lists them.");
  }
  BrowserWindowInterface* home = Home();
  if (!home || !SwitchWindowToSpace(home, profile_, *space)) {
    return Error("The window couldn't switch.");
  }
  return Text(base::StrCat(
      {"The user's window shows \"", Utf8(space->name()), "\" now."}));
}

Result ClaudeTabs::CreateSpace(const base::DictValue& input) {
  const std::string* name = input.FindString("name");
  SpaceService* service = SpaceService::Get(profile_);
  if (!name || base::TrimWhitespaceASCII(*name, base::TRIM_ALL).empty() ||
      !service) {
    return Error("Give the space a name.");
  }
  SpaceList& list = service->space_list();
  SpaceModel* space =
      list.AddSpace(list.spaces().size(), base::UTF8ToUTF16(*name));
  std::string text = base::StrCat(
      {"Created space \"", *name, "\" (", SpaceId(*space), ")."});
  if (input.FindBool("show").value_or(false)) {
    BrowserWindowInterface* home = Home();
    if (home && SwitchWindowToSpace(home, profile_, *space)) {
      text += " The user's window shows it.";
    }
  }
  return Text(std::move(text));
}

void ClaudeTabs::SearchHistory(const base::DictValue& input,
                               base::OnceCallback<void(Result)> done) {
  history::HistoryService* history = HistoryServiceFactory::GetForProfile(
      profile_, ServiceAccessType::EXPLICIT_ACCESS);
  if (!history) {
    std::move(done).Run(Error("History isn't available."));
    return;
  }
  history::QueryOptions options;
  options.max_count = std::clamp(input.FindInt("max_results").value_or(20), 1,
                                 100);
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

Result ClaudeTabs::ListDownloads(const base::DictValue& input) {
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

Result ClaudeTabs::SendTabToDevice(const base::DictValue& input,
                                   const Context& context) {
  send_tab_to_self::SendTabToSelfSyncService* service =
      SendTabToSelfSyncServiceFactory::GetForProfile(profile_);
  send_tab_to_self::SendTabToSelfModel* model =
      service ? service->GetSendTabToSelfModel() : nullptr;
  if (!model || !model->IsReady()) {
    return Error("Sending tabs needs Compass sync with another device.");
  }
  std::vector<send_tab_to_self::TargetDeviceInfo> devices =
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

}  // namespace compass::claude
