// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_DEV_MODE_DEV_MODE_BAR_H_
#define COMPASS_BROWSER_UI_VIEWS_DEV_MODE_DEV_MODE_BAR_H_

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class ContentsContainerView;

namespace views {
class ImageButton;
class Textfield;
}  // namespace views

namespace compass {

// Development mode's bar (docs/design/dev-mode.md): at the top of a pane
// whose page is in development mode, the page's full URL and buttons for
// DevTools' Console and Network panels, picking an element to inspect, and
// copying the URL. Each acts on the pane's page, so a split works. A child
// of the pane's ContentsContainerView, which gives it the top
// (GetContentsTopBarHeight()); DevModeController adds and removes it.
class DevModeBar : public views::View,
                   public content::WebContentsObserver,
                   public views::ViewObserver {
  METADATA_HEADER(DevModeBar, views::View)

 public:
  static constexpr int kHeight = 32;

  explicit DevModeBar(ContentsContainerView* container);
  DevModeBar(const DevModeBar&) = delete;
  DevModeBar& operator=(const DevModeBar&) = delete;
  ~DevModeBar() override;

  // The pane's bar, if it has one.
  static DevModeBar* FindIn(const views::View* container);

  // The page the bar shows and acts on.
  void SetWebContents(content::WebContents* contents);

  // The buttons' actions, for tests and the command bar.
  void OpenConsole();
  void OpenNetwork();
  void InspectElement();
  void CopyUrl();

  views::Textfield* url_field_for_testing() { return url_; }

  // views::View:
  void OnPaintBackground(gfx::Canvas* canvas) override;
  void OnThemeChanged() override;

  // content::WebContentsObserver:
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void WebContentsDestroyed() override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  void UpdateUrl();
  void UpdateIcons();
  void PlaceInContainer();
  void ShowCopied(bool copied);

  raw_ptr<ContentsContainerView> container_;
  raw_ptr<views::Textfield> url_ = nullptr;
  raw_ptr<views::ImageButton> console_ = nullptr;
  raw_ptr<views::ImageButton> network_ = nullptr;
  raw_ptr<views::ImageButton> inspect_ = nullptr;
  raw_ptr<views::ImageButton> copy_ = nullptr;
  // The copy button's check, after copying, until this fires.
  bool copied_ = false;
  base::OneShotTimer copied_timer_;
  base::ScopedObservation<views::View, views::ViewObserver>
      container_observation_{this};
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_DEV_MODE_DEV_MODE_BAR_H_
