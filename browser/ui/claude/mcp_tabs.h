// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_MCP_TABS_H_
#define COMPASS_BROWSER_UI_CLAUDE_MCP_TABS_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/types/expected.h"
#include "compass/browser/claude/claude_tools.h"

class BrowserWindowInterface;
class Profile;

namespace compass::claude {

class ClaudeTabs;

// The browser tools' view for an MCP client: every profile that allows MCP
// clients, each through its own ClaudeTabs. McpSession works out the profile
// each call acts in (ResolveCall()), checks the client may act there, and
// points this at it (SetCall()); the call then runs on that profile's
// ClaudeTabs, as it would for one profile. list_tabs lists every profile.
// See docs/design/claude.md, "MCP server".
class McpTabs : public TabsDelegate {
 public:
  // The profiles that allow MCP clients now.
  using ProfilesGetter = base::RepeatingCallback<std::vector<Profile*>()>;
  // Why list_tabs leaves a profile's tabs out (the client isn't allowed
  // there yet, say), or nullopt to list them.
  using HiddenReason =
      base::RepeatingCallback<std::optional<std::string>(Profile*)>;

  McpTabs(ProfilesGetter profiles, HiddenReason hidden);
  McpTabs(const McpTabs&) = delete;
  McpTabs& operator=(const McpTabs&) = delete;
  ~McpTabs() override;

  // The last active normal window among |profiles|' (not a popup, a parking
  // window or one swapping away), or null. |with_active_tab|: one that has
  // an active tab.
  static BrowserWindowInterface* FrontWindow(
      const std::vector<Profile*>& profiles,
      bool with_active_tab = false);
  // Whether MCP clients get a `profile` argument for tool |name|.
  static bool TakesProfile(const std::string& name);
  // |profile|'s name, as the profile menu shows it.
  static std::string NameOf(Profile* profile);

  // The profile a call acts in: the one its `profile`, ids or `space` name,
  // else |target|'s (the tab the page tools act on, if any), else the front
  // window's. An error if they name different profiles, or a name or id
  // matches in more than one. Without |name_profiles| (a client allowed in
  // no profile yet), errors don't name profiles. A space name isn't looked
  // up in profiles |denied| says the client was denied in: nothing tells it
  // what their spaces are called.
  base::expected<Profile*, std::string> ResolveCall(
      const std::string& name,
      const base::DictValue& input,
      content::WebContents* target,
      bool name_profiles,
      base::FunctionRef<bool(Profile*)> denied);
  // The next call acts in |profile|; |input| is its arguments (list_tabs's
  // `profile`, open_tab's `space`); |front_profile| is the profile of the
  // front window when the call came, before any question (switch_space's
  // swap).
  void SetCall(Profile* profile,
               const base::DictValue& input,
               Profile* front_profile);
  Profile* current() { return current_; }
  // |profile| stopped allowing MCP clients, or is going.
  void RemoveProfile(Profile* profile);

  // TabsDelegate:
  bool BrowserScopeAllowed() override;
  std::string ListTabs(const Context& context) override;
  content::WebContents* FindLiveTab(const std::string& id) override;
  base::expected<content::WebContents*, std::string> LoadTab(
      const std::string& id) override;
  content::WebContents* OpenTab(const GURL& url, bool active) override;
  std::string IdOf(content::WebContents* contents) override;
  void RunTool(const std::string& name,
               base::DictValue input,
               const Context& context,
               base::OnceCallback<void(Result)> done) override;

 private:
  // |profile|'s ClaudeTabs, made on first use.
  ClaudeTabs& TabsOf(Profile* profile);
  // The profiles in the space switcher's order.
  std::vector<Profile*> OrderedProfiles();
  // switch_space to a space of |current_|, shown in |front|, another
  // profile's window.
  Result SwapFrontWindow(BrowserWindowInterface* front,
                         const base::DictValue& input);

  ProfilesGetter profiles_;
  HiddenReason hidden_;
  std::map<raw_ptr<Profile>, std::unique_ptr<ClaudeTabs>> tabs_;
  // What SetCall() said.
  raw_ptr<Profile> current_ = nullptr;
  bool only_current_ = false;
  std::string space_;
  raw_ptr<Profile> front_profile_ = nullptr;
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_MCP_TABS_H_
