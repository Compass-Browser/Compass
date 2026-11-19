// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_CLAUDE_TABS_ANDROID_H_
#define COMPASS_BROWSER_ANDROID_CLAUDE_TABS_ANDROID_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "compass/browser/claude/claude_tools.h"

class Profile;
class TabAndroid;

namespace compass {

class SidebarNode;
class SpaceModel;
class SpacesAndroid;

// The browser tools on Android (docs/design/android.md, "Claude"): the
// desktop's (ClaudeTabs, docs/design/claude.md, "Browser tools") over the
// profile's sidebar (SpacesAndroid) and Chrome's tab models. Ids are the
// desktop's: "n" and 8 hex digits for sidebar entries, "s" and 8 for spaces,
// "t" and the Android tab ID for tabs outside the sidebar.
class ClaudeTabsAndroid : public claude::TabsDelegate {
 public:
  // The chat's window, in Java (ClaudeChat): what it shows, and showing.
  class Window {
   public:
    virtual ~Window() = default;
    // The space the window shows (its UUID), or empty.
    virtual std::string ShownSpace() = 0;
    // Shows |node_id| (a tab entry's UUID) in the window: its tab, or a new
    // one for it; |space_id| is its space, which the window switches to
    // first (empty for a favorite).
    virtual void ShowNode(const std::string& node_id,
                          const std::string& url,
                          const std::string& space_id) = 0;
    // Opens |node_id| in a background tab of the window.
    virtual void LoadInBackground(const std::string& node_id,
                                  const std::string& url) = 0;
    // The window shows |space_id| (one of its profile's).
    virtual void SwitchSpace(const std::string& space_id) = 0;
  };

  // |window| outlives this.
  ClaudeTabsAndroid(Profile* profile, Window* window);
  ClaudeTabsAndroid(const ClaudeTabsAndroid&) = delete;
  ClaudeTabsAndroid& operator=(const ClaudeTabsAndroid&) = delete;
  ~ClaudeTabsAndroid() override;

  // claude::TabsDelegate:
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
  SpacesAndroid* spaces();
  // The sidebar node |id| names, or null; |space| gets its space.
  SidebarNode* FindNode(const std::string& id, SpaceModel** space = nullptr);
  // A space by name (any case) or id.
  SpaceModel* FindSpace(const std::string& name_or_id);
  // The space the chat's window shows.
  SpaceModel* ShownSpace();
  // |node|'s tab, if it has one.
  TabAndroid* TabOf(const SidebarNode& node);

  Result CloseTab(const base::DictValue& input);
  Result ActivateTab(const base::DictValue& input);
  Result MoveTab(const base::DictValue& input);
  Result RenameTab(const base::DictValue& input);
  Result CreateFolder(const base::DictValue& input);
  Result MuteTab(const base::DictValue& input);
  Result SplitTabs(const base::DictValue& input);
  Result SwitchSpace(const base::DictValue& input);
  Result CreateSpace(const base::DictValue& input);
  void SearchHistory(const base::DictValue& input,
                     base::OnceCallback<void(Result)> done);
  Result ListDownloads(const base::DictValue& input);
  Result SendTabToDevice(const base::DictValue& input, const Context& context);

  const raw_ptr<Profile> profile_;
  const raw_ptr<Window> window_;
  base::CancelableTaskTracker history_tracker_;
  base::WeakPtrFactory<ClaudeTabsAndroid> weak_factory_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_CLAUDE_TABS_ANDROID_H_
