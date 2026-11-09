// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ANDROID_CLAUDE_TABS_ANDROID_H_
#define COMPASS_BROWSER_ANDROID_CLAUDE_TABS_ANDROID_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "compass/browser/claude/claude_tools.h"

class Profile;
class TabAndroid;

namespace compass {

// The browser tools on Android (docs/design/android.md, "Claude"): the
// profile's regular tabs in Chrome's tab models, every window's. A tab's
// list_tabs id is its Android tab ID. list_tabs, open_tab, select_tab and
// close_tab; the desktop's others (spaces, folders, splits) aren't here.
class ClaudeTabsAndroid : public claude::TabsDelegate {
 public:
  explicit ClaudeTabsAndroid(Profile* profile);
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
  TabAndroid* FindTab(const std::string& id);

  const raw_ptr<Profile> profile_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_ANDROID_CLAUDE_TABS_ANDROID_H_
