// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_CLAUDE_CLAUDE_PANEL_H_
#define COMPASS_BROWSER_UI_VIEWS_CLAUDE_CLAUDE_PANEL_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;
class SidePanelEntryScope;

namespace content {
class WebContents;
}  // namespace content

namespace views {
class View;
}  // namespace views

namespace compass::claude {

// A window's chat with Claude: chrome://compass-claude in the side panel,
// on the right (the sidebar is on the left), registered as a window-wide
// entry the first time it opens. It has no upstream header: the page draws
// its own. Owned by the window's sidebar view. See docs/design/claude.md,
// "Chat panel".
class ClaudePanel {
 public:
  DECLARE_USER_DATA(ClaudePanel);

  explicit ClaudePanel(BrowserWindowInterface* browser);
  ClaudePanel(const ClaudePanel&) = delete;
  ClaudePanel& operator=(const ClaudePanel&) = delete;
  ~ClaudePanel();

  static ClaudePanel* From(BrowserWindowInterface* browser);

  void Show();
  void Close();
  // Cmd+Shift+E.
  void Toggle();
  bool IsShowing() const;

  // The page's contents while the panel shows; null otherwise.
  content::WebContents* web_contents_for_testing();

 private:
  void RegisterEntryIfNeeded();
  std::unique_ptr<views::View> CreateView(SidePanelEntryScope& scope);

  const raw_ptr<BrowserWindowInterface> browser_;
  bool registered_ = false;
  ui::ScopedUnownedUserData<ClaudePanel> scoped_data_holder_;
};

// Offers browser command |id| to the panel: kToggleClaudeCommand. Returns
// true if it handled it.
bool HandleClaudeCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_VIEWS_CLAUDE_CLAUDE_PANEL_H_
