// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HOVER_CARD_H_
#define COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HOVER_CARD_H_

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_thumbnail_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class TabHoverCardBubbleView;

namespace gfx {
class ImageSkia;
}  // namespace gfx

namespace tabs {
struct TabData;
}  // namespace tabs

namespace compass {

class SidebarHoverCardTarget;

// The hover card of the sidebar's entries: upstream's tab hover card bubble
// (title, domain, a preview of a live tab), next to the hovered row. The page
// says which row that is (SidebarPageHandler::ShowHoverCard()). Lives on the
// sidebar page's WebContents, which CompassSidebarView owns. See
// docs/design/sidebar.md, "Hover cards".
class SidebarHoverCard
    : public content::WebContentsUserData<SidebarHoverCard>,
      public views::ViewObserver {
 public:
  SidebarHoverCard(const SidebarHoverCard&) = delete;
  SidebarHoverCard& operator=(const SidebarHoverCard&) = delete;
  ~SidebarHoverCard() override;

  // Shows the card for a tab with |data| next to the row at |row_top| and
  // |row_height| in the sidebar, or moves it there. |show_preview| asks for a
  // screenshot of the live tab, which it gets if hover card images are on (see
  // ArePreviewsEnabled() in the .cc) and the tab has a thumbnail.
  void Show(const tabs::TabData& data,
            int row_top,
            int row_height,
            bool show_preview);
  void Hide();

  bool IsShowing() const { return !!bubble_; }
  TabHoverCardBubbleView* bubble_for_testing() { return bubble_; }

  // views::ViewObserver:
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  friend WebContentsUserData;

  // |sidebar| hosts |contents|, and outlives it.
  SidebarHoverCard(content::WebContents* contents, views::View* sidebar);

  void OnPreviewImageAvailable(TabHoverCardThumbnailObserver* observer,
                               gfx::ImageSkia preview);

  const raw_ptr<views::View> sidebar_;
  // The card's anchor, a child of |sidebar_| over the hovered row. Created
  // with the first card.
  raw_ptr<SidebarHoverCardTarget> target_ = nullptr;
  // Owned by its widget.
  raw_ptr<TabHoverCardBubbleView> bubble_ = nullptr;
  // Whether |bubble_| was made with room for a preview.
  bool bubble_has_previews_ = false;
  base::ScopedObservation<views::View, views::ViewObserver> bubble_observation_{
      this};

  TabHoverCardThumbnailObserver thumbnail_observer_;
  base::CallbackListSubscription thumbnail_subscription_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HOVER_CARD_H_
