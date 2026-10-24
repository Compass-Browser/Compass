// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STRATA_BROWSER_UI_VIEWS_COMMAND_BAR_COMMAND_BAR_H_
#define STRATA_BROWSER_UI_VIEWS_COMMAND_BAR_COMMAND_BAR_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "strata/browser/ui/webui/command_bar/command_bar.mojom.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;
class WebUIBubbleDialogView;
class WebUIContentsWrapper;

namespace content {
class WebContents;
}  // namespace content

namespace strata {

class CommandBarPageHandler;

// A window's command bar: chrome://strata-command-bar in a bubble centered
// over the top of the content area. The page stays loaded between openings,
// so it opens at once. Owned by the window's sidebar view. See
// docs/design/command-bar.md.
class CommandBar {
 public:
  DECLARE_USER_DATA(CommandBar);

  explicit CommandBar(BrowserWindowInterface* browser);
  CommandBar(const CommandBar&) = delete;
  CommandBar& operator=(const CommandBar&) = delete;
  ~CommandBar();

  static CommandBar* From(BrowserWindowInterface* browser);

  // Opens the bar in |mode|: empty for a new entry, on the active tab's URL
  // for the current one. If it's open already, starts over in |mode|.
  void Show(command_bar::mojom::Mode mode);
  void Close();
  bool IsShowing() const;

  // Loads the page ahead of the first Show().
  void Preload();

  // The page's handler connected: tells it what Show() asked for, if the bar
  // is opening.
  void OnPageHandlerCreated(CommandBarPageHandler* handler);

  // The page's contents; null before Show() or Preload().
  content::WebContents* web_contents_for_testing();

 private:
  WebUIContentsWrapper* GetOrCreateContents();
  // Null until the page has connected.
  CommandBarPageHandler* GetPageHandler();

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<WebUIContentsWrapper> contents_;
  base::WeakPtr<WebUIBubbleDialogView> bubble_;
  command_bar::mojom::Mode mode_ = command_bar::mojom::Mode::kNewEntry;
  std::u16string text_;

  ui::ScopedUnownedUserData<CommandBar> scoped_data_holder_;
};

// Offers browser command |id| to Strata before upstream runs it. Called from
// BrowserCommandController (see patches/). Returns true if Strata handled it:
// IDC_NEW_TAB and IDC_FOCUS_LOCATION open the command bar.
bool HandleBrowserCommand(BrowserWindowInterface* browser, int id);

}  // namespace strata

#endif  // STRATA_BROWSER_UI_VIEWS_COMMAND_BAR_COMMAND_BAR_H_
