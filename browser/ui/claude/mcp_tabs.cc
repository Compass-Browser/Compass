// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/mcp_tabs.h"

#include <algorithm>
#include <utility>

#include "base/i18n/case_conversion.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/claude/claude_tabs.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"

namespace compass::claude {

namespace {

using Result = TabsDelegate::Result;

// The arguments that name a sidebar entry or a tab. create_folder's `in` is
// a folder id, or pinned or today.
constexpr const char* kIdKeys[] = {"tab_id", "with_tab_id", "folder_id", "in"};

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

std::string Quoted(Profile* profile) {
  return base::StrCat({"\"", McpTabs::NameOf(profile), "\""});
}

}  // namespace

McpTabs::McpTabs(ProfilesGetter profiles, HiddenReason hidden)
    : profiles_(std::move(profiles)), hidden_(std::move(hidden)) {}

McpTabs::~McpTabs() = default;

// static
BrowserWindowInterface* McpTabs::FrontWindow(
    const std::vector<Profile*>& profiles) {
  BrowserWindowInterface* found = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (!std::ranges::contains(profiles, browser->GetProfile()) ||
            browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
          return true;
        }
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features &&
            (features->is_parking_window() || features->swapped_away())) {
          return true;
        }
        found = browser;
        return false;
      });
  return found;
}

// static
std::string McpTabs::NameOf(Profile* profile) {
  return base::UTF16ToUTF8(SpaceDirectory::GetProfileName(profile));
}

ClaudeTabs& McpTabs::TabsOf(Profile* profile) {
  std::unique_ptr<ClaudeTabs>& tabs = tabs_[profile];
  if (!tabs) {
    // No home window: tabs open in the profile's last active window.
    tabs = std::make_unique<ClaudeTabs>(profile, nullptr);
  }
  return *tabs;
}

std::vector<Profile*> McpTabs::OrderedProfiles() {
  const std::vector<Profile*> profiles = profiles_.Run();
  std::vector<Profile*> ordered;
  // By their first space in the switcher; profiles it doesn't list last.
  if (SpaceDirectory* directory = SpaceDirectory::GetIfExists()) {
    for (const SpaceDirectory::Entry& entry : directory->GetSpaces()) {
      Profile* profile = entry.profile;
      if (std::ranges::contains(profiles, profile) &&
          !std::ranges::contains(ordered, profile)) {
        ordered.push_back(profile);
      }
    }
  }
  for (Profile* profile : profiles) {
    if (!std::ranges::contains(ordered, profile)) {
      ordered.push_back(profile);
    }
  }
  return ordered;
}

base::expected<Profile*, std::string> McpTabs::ResolveCall(
    const base::DictValue& input,
    content::WebContents* target) {
  const std::vector<Profile*> profiles = profiles_.Run();
  Profile* found = nullptr;
  std::string found_by;
  // Each argument that names a profile must name the same one: nothing
  // moves or combines across profiles, as in the sidebar.
  auto take = [&](Profile* profile,
                  const std::string& by) -> std::optional<std::string> {
    if (found && found != profile) {
      return base::StrCat({found_by, " is in profile ", Quoted(found), " and ",
                           by, " in profile ", Quoted(profile),
                           ": tools don't act across profiles."});
    }
    found = profile;
    found_by = by;
    return std::nullopt;
  };

  if (const std::string* wanted = input.FindString("profile")) {
    const std::u16string lower =
        base::i18n::ToLower(base::UTF8ToUTF16(*wanted));
    std::vector<Profile*> matches;
    std::string names;
    for (Profile* profile : profiles) {
      const std::u16string profile_name =
          SpaceDirectory::GetProfileName(profile);
      if (base::i18n::ToLower(profile_name) == lower) {
        matches.push_back(profile);
      }
      base::StrAppend(&names, {names.empty() ? "" : ", ", Quoted(profile)});
    }
    if (matches.empty()) {
      return base::unexpected(
          base::StrCat({"No profile \"", *wanted,
                        "\" allows MCP clients. Those that do: ", names, "."}));
    }
    if (matches.size() > 1) {
      return base::unexpected(base::StrCat(
          {"Several profiles are called \"", *wanted,
           "\". Leave out profile and ask the user to bring a window of the "
           "one they mean to the front; tools that take a tab or space id "
           "also find its profile from the id."}));
    }
    if (std::optional<std::string> error =
            take(matches.front(), "profile \"" + *wanted + "\"")) {
      return base::unexpected(*error);
    }
  }

  for (const char* key : kIdKeys) {
    const std::string* id = input.FindString(key);
    if (!id || *id == "pinned" || *id == "today") {
      continue;
    }
    std::vector<Profile*> owners;
    for (Profile* profile : profiles) {
      if (TabsOf(profile).Owns(*id)) {
        owners.push_back(profile);
      }
    }
    if (owners.size() > 1) {
      return base::unexpected(base::StrCat(
          {*id,
           " names an entry in more than one profile. Run list_tabs "
           "and use the tab instead, or give profile."}));
    }
    if (owners.empty()) {
      // No such id: the profile's own tool says so.
      continue;
    }
    if (std::optional<std::string> error = take(owners.front(), *id)) {
      return base::unexpected(*error);
    }
  }

  if (const std::string* space = input.FindString("space")) {
    std::vector<Profile*> owners;
    for (Profile* profile : profiles) {
      if (TabsOf(profile).FindSpace(*space)) {
        owners.push_back(profile);
      }
    }
    if (owners.empty()) {
      return base::unexpected(
          base::StrCat({"No space \"", *space, "\". list_tabs lists them."}));
    }
    if (found && std::ranges::contains(owners, found)) {
      // Another argument already named the profile: the name isn't a guess
      // there.
      owners = {found};
    }
    if (owners.size() > 1) {
      return base::unexpected(
          base::StrCat({"More than one profile has a space \"", *space,
                        "\": give its id (s…) from list_tabs."}));
    }
    if (std::optional<std::string> error =
            take(owners.front(), "space \"" + *space + "\"")) {
      return base::unexpected(*error);
    }
  }

  if (found) {
    return found;
  }
  if (target) {
    Profile* profile = Profile::FromBrowserContext(target->GetBrowserContext());
    if (std::ranges::contains(profiles, profile)) {
      return profile;
    }
  }
  if (BrowserWindowInterface* front = FrontWindow(profiles)) {
    return front->GetProfile();
  }
  if (profiles.size() == 1) {
    // A profile kept loaded by the space list, with no window.
    return profiles.front();
  }
  return base::unexpected(std::string(
      "Open a Compass window of a profile that allows MCP clients, or give "
      "profile."));
}

void McpTabs::SetCall(Profile* profile, const base::DictValue& input) {
  current_ = profile;
  // As ResolveCall() reads it: a profile that isn't a string names none.
  only_current_ = input.FindString("profile") != nullptr;
  const std::string* space = input.FindString("space");
  space_ = space ? *space : std::string();
}

void McpTabs::RemoveProfile(Profile* profile) {
  tabs_.erase(profile);
  if (current_ == profile) {
    current_ = nullptr;
  }
}

bool McpTabs::BrowserScopeAllowed() {
  return current_ && current_->GetPrefs()->GetBoolean(prefs::kBrowserScope);
}

std::string McpTabs::ListTabs(const Context& context) {
  if (!current_) {
    return "No tabs.";
  }
  const std::vector<Profile*> profiles =
      only_current_ ? std::vector<Profile*>{current_.get()} : OrderedProfiles();
  if (profiles.size() == 1) {
    // One profile reads as it always did.
    return TabsOf(profiles.front()).ListTabs(context);
  }
  std::string text;
  for (Profile* profile : profiles) {
    if (!text.empty()) {
      text += "\n";
    }
    text += "Profile " + Quoted(profile);
    if (profile == current_) {
      text += " [current]";
    }
    std::optional<std::string> hidden = hidden_.Run(profile);
    if (!hidden && !profile->GetPrefs()->GetBoolean(prefs::kBrowserScope)) {
      hidden = "\"Let Claude use the whole browser\" is off there";
    }
    if (hidden) {
      base::StrAppend(&text, {": ", *hidden, ".\n"});
      continue;
    }
    base::StrAppend(&text, {":\n", TabsOf(profile).ListTabs(context)});
  }
  return text;
}

content::WebContents* McpTabs::FindLiveTab(const std::string& id) {
  return current_ ? TabsOf(current_).FindLiveTab(id) : nullptr;
}

base::expected<content::WebContents*, std::string> McpTabs::LoadTab(
    const std::string& id) {
  if (!current_) {
    return base::unexpected(std::string("No profile allows MCP clients."));
  }
  return TabsOf(current_).LoadTab(id);
}

content::WebContents* McpTabs::OpenTab(const GURL& url, bool active) {
  if (!current_) {
    return nullptr;
  }
  ClaudeTabs& tabs = TabsOf(current_);
  if (!space_.empty()) {
    SpaceModel* space = tabs.FindSpace(space_);
    return space ? tabs.OpenTabInSpace(*space, url, active) : nullptr;
  }
  return tabs.OpenTab(url, active);
}

std::string McpTabs::IdOf(content::WebContents* contents) {
  Profile* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  if (!std::ranges::contains(profiles_.Run(), profile)) {
    profile = current_;
  }
  return profile ? TabsOf(profile).IdOf(contents) : std::string();
}

void McpTabs::RunTool(const std::string& name,
                      base::DictValue input,
                      const Context& context,
                      base::OnceCallback<void(Result)> done) {
  if (!current_) {
    std::move(done).Run(Error("No profile allows MCP clients."));
    return;
  }
  if (name == "send_tab_to_device" && input.contains("device") &&
      !input.contains("tab_id") && context.target &&
      Profile::FromBrowserContext(context.target->GetBrowserContext()) !=
          current_) {
    // It would send that tab through another profile's devices.
    std::move(done).Run(Error(base::StrCat(
        {"The tab you act on isn't in profile ", Quoted(current_),
         ": give a tab_id of that profile, or leave out profile."})));
    return;
  }
  if (name == "switch_space") {
    // A space of another profile than the front window's: that window
    // swaps in place, as the switcher does.
    BrowserWindowInterface* front = FrontWindow(profiles_.Run());
    if (front && front->GetProfile() != current_) {
      std::move(done).Run(SwapFrontWindow(front, input));
      return;
    }
  }
  TabsOf(current_).RunTool(name, std::move(input), context, std::move(done));
}

Result McpTabs::SwapFrontWindow(BrowserWindowInterface* front,
                                const base::DictValue& input) {
  const std::string* name = input.FindString("space");
  SpaceModel* space = name ? TabsOf(current_).FindSpace(*name) : nullptr;
  if (!space) {
    return Error("No such space. list_tabs lists them.");
  }
  if (!SwitchWindowToSpace(front, current_, *space)) {
    return Error("The window couldn't switch.");
  }
  return Text(base::StrCat({"The user's window shows \"",
                            base::UTF16ToUTF8(space->name()), "\" of profile ",
                            Quoted(current_), " now."}));
}

}  // namespace compass::claude
