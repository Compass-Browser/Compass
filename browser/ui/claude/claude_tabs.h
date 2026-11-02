// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_CLAUDE_TABS_H_
#define COMPASS_BROWSER_UI_CLAUDE_CLAUDE_TABS_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "compass/browser/claude/claude_tools.h"

class BrowserWindowInterface;
class Profile;

namespace compass {
class SidebarNode;
class SpaceModel;
}  // namespace compass

namespace compass::claude {

// The browser tools' view of a profile (docs/design/claude.md, D5 and D7):
// its sidebar in every space (favorites, pinned entries in folders, Today),
// loaded or not, the tabs of its other windows, and its history, downloads
// and devices. Entries are named by their node ("n" and the first 8 hex
// digits of its ID); tabs without one (a popup's) by their handle ("t…").
// Tabs open in |home|, the chat's window, in its space; or in the last
// active window if |home| is gone.
class ClaudeTabs : public TabsDelegate {
 public:
  ClaudeTabs(Profile* profile, BrowserWindowInterface* home);
  ClaudeTabs(const ClaudeTabs&) = delete;
  ClaudeTabs& operator=(const ClaudeTabs&) = delete;
  ~ClaudeTabs() override;

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
  BrowserWindowInterface* Home();
  // The sidebar node |id| names, in any of the profile's spaces or its
  // favorites, or null; |space| gets its space.
  SidebarNode* FindNode(const std::string& id, SpaceModel** space = nullptr);
  // A space by name (any case) or id.
  SpaceModel* FindSpace(const std::string& name_or_id);
  // The window showing |space|, or null.
  BrowserWindowInterface* WindowShowing(const SpaceModel& space);

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
  // Compared, never used, once its window is gone.
  const raw_ptr<BrowserWindowInterface, DisableDanglingPtrDetection> home_;
  base::CancelableTaskTracker history_tracker_;
  base::WeakPtrFactory<ClaudeTabs> weak_factory_{this};
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_CLAUDE_TABS_H_
