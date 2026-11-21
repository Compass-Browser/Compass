// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_FRAME_SPACE_FADE_H_
#define COMPASS_BROWSER_UI_VIEWS_FRAME_SPACE_FADE_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "compass/browser/window_sync/window_features.h"

class BrowserWindowInterface;
class ContentsWebView;

namespace ui {
class LayerTreeOwner;
}  // namespace ui

namespace views {
class View;
}  // namespace views

namespace compass {

// The pages fade from one space to the next as the window switches, as in
// Arc, rather than cut. While a swipe pulls in another space, the pages fade
// with the fingers into the frame's color. Just before the switch, a copy of
// each shown pane's layers (the page's last frame: a surface layer's clone
// shows the same surface) goes over the window and fades out, while the new
// space's pages fade in. See docs/design/spaces.md, "Switching, step by
// step".
class SpaceFade : public WindowFeatures::Observer {
 public:
  static constexpr base::TimeDelta kDuration = base::Milliseconds(220);

  SpaceFade(views::View* browser_view, BrowserWindowInterface* browser);
  SpaceFade(const SpaceFade&) = delete;
  SpaceFade& operator=(const SpaceFade&) = delete;
  ~SpaceFade() override;

  // Whether the old space's pages show over the new ones, for tests.
  bool IsFadingForTesting() const { return !!copy_; }

  // WindowFeatures::Observer:
  void OnSpaceWillSwitch() override;
  void OnSpaceSwitched() override;
  void OnSpacePreviewChanged() override;

 private:
  // The window's panes (one, or a split's), shown or not.
  std::vector<ContentsWebView*> GetPanes() const;
  // Ends a fade: the copy goes, and the panes are opaque again.
  void Clear();

  const raw_ptr<views::View> browser_view_;
  const raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<WindowFeatures> features_ = nullptr;
  base::ScopedObservation<WindowFeatures, WindowFeatures::Observer>
      features_observation_{this};
  // The old space's panes, over the window's other layers while they fade.
  std::unique_ptr<ui::LayerTreeOwner> copy_;
  base::OneShotTimer clear_timer_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_FRAME_SPACE_FADE_H_
