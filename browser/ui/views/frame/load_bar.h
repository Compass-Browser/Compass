// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_FRAME_LOAD_BAR_H_
#define COMPASS_BROWSER_UI_VIEWS_FRAME_LOAD_BAR_H_

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class BrowserWindowInterface;

namespace compass {

// The active tab's load progress: a thin line in the frame's gap above the
// page's card, as wide as the card between its corners. It fills as the page
// loads; while the tab waits for the server's first answer (connecting), a
// segment sweeps back and forth instead. Once the page has loaded it fills
// and fades. See docs/design/window-frame.md, "Load bar".
class LoadBar : public views::View,
                public views::ViewObserver,
                public content::WebContentsObserver {
  METADATA_HEADER(LoadBar, views::View)

 public:
  // Adds one to |browser_view|, over its other children.
  static LoadBar* Install(views::View* browser_view,
                          BrowserWindowInterface* browser);

  explicit LoadBar(BrowserWindowInterface* browser);
  LoadBar(const LoadBar&) = delete;
  LoadBar& operator=(const LoadBar&) = delete;
  ~LoadBar() override;

  // Whether the sweep shows (the tab is connecting), for tests.
  bool IsSweepingForTesting() const { return shown_ && waiting_; }
  bool IsShownForTesting() const { return shown_; }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

  // views::ViewObserver:
  void OnChildViewAdded(views::View* observed_view,
                        views::View* child) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  // content::WebContentsObserver:
  void DidStartLoading() override;
  void DidStopLoading() override;
  void LoadProgressChanged(double progress) override;

 private:
  void OnActiveTabChanged(BrowserWindowInterface* browser);
  // Shows or finishes the bar from the tab's loading state.
  void Update();
  void Tick();
  // Over the card's top edge, in the frame's gap.
  void UpdateBounds();

  const raw_ptr<BrowserWindowInterface> browser_;
  base::CallbackListSubscription active_tab_subscription_;
  base::ScopedObservation<views::View, views::ViewObserver>
      host_observation_{this};
  base::RepeatingTimer timer_;
  base::TimeTicks last_tick_;

  // Showing: from a load's start until it has faded after its end.
  bool shown_ = false;
  // The tab is waiting for the server's first answer.
  bool waiting_ = false;
  // The load is over: the line fills, then fades.
  bool finishing_ = false;
  // The line's length, 0 to 1, following the load's progress.
  double progress_ = 0;
  // Where the sweep is, in seconds since it began.
  double sweep_time_ = 0;
  double opacity_ = 1;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_FRAME_LOAD_BAR_H_
