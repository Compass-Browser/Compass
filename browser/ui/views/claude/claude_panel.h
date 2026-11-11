// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_CLAUDE_CLAUDE_PANEL_H_
#define COMPASS_BROWSER_UI_VIEWS_CLAUDE_CLAUDE_PANEL_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/views/view_tracker.h"

class BrowserWindowInterface;
class SidePanelEntryScope;
template <class T>
class WebUIContentsWrapperT;

namespace content {
class WebContents;
}  // namespace content

namespace views {
class View;
}  // namespace views

namespace compass::claude {

class ClaudeChatPageHandler;
class ClaudeChatUI;

// A window's chat with Claude: chrome://compass-claude in the side panel,
// on the right (the sidebar is on the left), registered as a window-wide
// entry the first time it opens. It has no upstream header: the page draws
// its own. The page lives as long as the window: closing the panel only
// takes its view away (upstream's side panel drops a closed entry's view,
// and would take the conversation with it). Owned by the window's sidebar
// view. See docs/design/claude.md, "Chat panel".
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
  // Shows the panel and sends |question| as the user's message, with the
  // page attached (the command bar's "Ask Claude"). Waits for the page if
  // it isn't loaded yet.
  void Ask(const std::string& question);

  // The page's handler, while the page is connected.
  void SetPageHandler(ClaudeChatPageHandler* handler);
  // |handler| goes away; forgotten if it's still the page's.
  void RemovePageHandler(ClaudeChatPageHandler* handler);
  // The question asked before the page connected, once.
  std::string TakeQuestion();

  // The page's contents while the panel shows; null otherwise. (Upstream's
  // SidePanelUI::GetWebContentsForTest() would make a new page each call.)
  content::WebContents* web_contents_for_testing();

 private:
  void RegisterEntryIfNeeded();
  std::unique_ptr<views::View> CreateView(SidePanelEntryScope& scope);

  const raw_ptr<BrowserWindowInterface> browser_;
  bool registered_ = false;
  raw_ptr<ClaudeChatPageHandler> page_handler_ = nullptr;
  // The page, made on the first show and kept while the window lives; each
  // show wraps it in a new view.
  std::unique_ptr<WebUIContentsWrapperT<ClaudeChatUI>> contents_;
  // The panel's view, while it exists.
  views::ViewTracker view_;
  std::string pending_question_;
  ui::ScopedUnownedUserData<ClaudePanel> scoped_data_holder_;
};

// Offers browser command |id| to the panel: kToggleClaudeCommand. Returns
// true if it handled it.
bool HandleClaudeCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_UI_VIEWS_CLAUDE_CLAUDE_PANEL_H_
