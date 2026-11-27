// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_COMMAND_BAR_COMMAND_BAR_H_
#define COMPASS_BROWSER_UI_VIEWS_COMMAND_BAR_COMMAND_BAR_H_

#include "compass/browser/ui/compass_command_ids.h"
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "compass/browser/ui/webui/command_bar/command_bar.mojom.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/events/event_observer.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

class BrowserWindowInterface;
class WebUIBubbleDialogView;
class WebUIContentsWrapper;

namespace gfx {
class Point;
}  // namespace gfx

namespace views {
class EventMonitor;
}  // namespace views

namespace content {
class WebContents;
}  // namespace content

namespace compass {

class CommandBarPageHandler;

// A window's command bar: chrome://compass-command-bar in a bubble centered
// over the top of the content area. The page stays loaded between openings,
// so it opens at once. Owned by the window's sidebar view. See
// docs/design/command-bar.md.
//
// The bubble closes when it loses activation, as upstream's do. A bubble
// that never got it (shown while the window was still coming forward) would
// stay until clicked into and away, so a press anywhere else in the window
// closes it too, and the window coming forward hands it the activation.
class CommandBar : public views::WidgetObserver, public ui::EventObserver {
 public:
  DECLARE_USER_DATA(CommandBar);

  explicit CommandBar(BrowserWindowInterface* browser);
  CommandBar(const CommandBar&) = delete;
  CommandBar& operator=(const CommandBar&) = delete;
  ~CommandBar() override;

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

  // views::WidgetObserver, for the bubble's widget and the window's:
  void OnWidgetVisibilityChanged(views::Widget* widget, bool visible) override;
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetDestroying(views::Widget* widget) override;

  // ui::EventObserver: a press in the window, outside the bubble.
  void OnEvent(const ui::Event& event) override;

  // A press in the window at |screen_point|: closes the bar (soon) unless
  // it's on the bubble.
  void OnWindowPress(const gfx::Point& screen_point);

 private:
  // Gives the showing bubble the activation if its window has it.
  void ActivateIfWindowActive();
  void StopWatching();

  WebUIContentsWrapper* GetOrCreateContents();
  // Null until the page has connected.
  CommandBarPageHandler* GetPageHandler();

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<WebUIContentsWrapper> contents_;
  base::WeakPtr<WebUIBubbleDialogView> bubble_;
  command_bar::mojom::Mode mode_ = command_bar::mojom::Mode::kNewEntry;
  std::u16string text_;
  // While the bubble shows: presses in the window, and its activation.
  std::unique_ptr<views::EventMonitor> press_monitor_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      bubble_observation_{this};
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      window_observation_{this};

  ui::ScopedUnownedUserData<CommandBar> scoped_data_holder_;

  base::WeakPtrFactory<CommandBar> weak_factory_{this};
};

// Offers browser command |id| to Compass before upstream runs it. Called from
// BrowserCommandController (see patches/). Returns true if Compass handled it:
// IDC_NEW_TAB and IDC_FOCUS_LOCATION open the command bar. Compass's own
// commands (compass_command_ids.h) come here before upstream checks whether
// a command is enabled, which they never are.
bool HandleBrowserCommand(BrowserWindowInterface* browser, int id);

// Whether Compass enables browser command |id| where upstream may not:
// IDC_RESTORE_TAB while a closed sidebar entry can come back, which
// upstream's TabRestoreService may not know (an unloaded entry). Called from
// BrowserCommandController::IsCommandEnabled() (see patches/).
bool IsBrowserCommandEnabled(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_COMMAND_BAR_COMMAND_BAR_H_
