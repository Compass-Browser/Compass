// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_CLAUDE_CLAUDE_TABS_H_
#define COMPASS_BROWSER_UI_CLAUDE_CLAUDE_TABS_H_

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "compass/browser/claude/claude_tools.h"

class BrowserWindowInterface;
class Profile;

namespace compass::claude {

// The browser tools' view of a profile (docs/design/claude.md, D5): its
// tabs in every window, with their spaces, including the hidden parking
// window's (tabs of spaces no window shows). Tabs open in |home|, the chat's
// window, in its space; or in the last active window if |home| is gone.
class ClaudeTabs : public TabsDelegate {
 public:
  ClaudeTabs(Profile* profile, BrowserWindowInterface* home);
  ClaudeTabs(const ClaudeTabs&) = delete;
  ClaudeTabs& operator=(const ClaudeTabs&) = delete;
  ~ClaudeTabs() override;

  // TabsDelegate:
  bool BrowserScopeAllowed() override;
  std::vector<TabInfo> ListTabs() override;
  content::WebContents* FindTab(const std::string& id) override;
  content::WebContents* OpenTab(const GURL& url, bool active) override;
  std::string IdOf(content::WebContents* contents) override;
  void CloseTab(content::WebContents* contents) override;

 private:
  BrowserWindowInterface* Home();

  const raw_ptr<Profile> profile_;
  // Compared, never used, once its window is gone.
  const raw_ptr<BrowserWindowInterface, DisableDanglingPtrDetection> home_;
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_CLAUDE_CLAUDE_TABS_H_
