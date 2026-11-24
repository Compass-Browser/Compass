// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HOVER_CARD_H_
#define COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HOVER_CARD_H_

#include <memory>
#include <optional>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "compass/browser/ui/live_folders/tab_preview_service.h"
#include "compass/browser/ui/views/sidebar/tab_preview_view.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_thumbnail_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/mouse_watcher.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class TabHoverCardBubbleView;

namespace gfx {
class ImageSkia;
class Point;
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
      public views::ViewObserver,
      public views::MouseWatcherListener {
 public:
  SidebarHoverCard(const SidebarHoverCard&) = delete;
  SidebarHoverCard& operator=(const SidebarHoverCard&) = delete;
  ~SidebarHoverCard() override;

  // Shows the card for a tab with |data| next to the row at |row_top| and
  // |row_height| in the sidebar, or moves it there. |show_preview| asks for a
  // screenshot of the live tab, which it gets if hover card images are on (see
  // ArePreviewsEnabled() in the .cc) and the tab has a thumbnail.
  // |tab_contents| is the live tab's (null for an unloaded entry); unless
  // |is_active_tab|, a plugin that previews its page shows that instead of
  // the screenshot (docs/design/live-folders.md, "Tab previews"), and a
  // click on one of its rows runs |on_preview_click| (none: display only).
  void Show(const tabs::TabData& data,
            int row_top,
            int row_height,
            bool show_preview,
            content::WebContents* tab_contents = nullptr,
            bool is_active_tab = true,
            TabPreviewView::ItemCallback on_preview_click =
                TabPreviewView::ItemCallback());
  void Hide();
  // The pointer left the row. A card whose plugin rows take clicks stays
  // while the pointer is on its way to it or on it, and hides once it's
  // elsewhere; any other card hides now.
  void Release();

  bool IsShowing() const { return !!bubble_; }
  TabHoverCardBubbleView* bubble_for_testing() { return bubble_; }

  // views::ViewObserver:
  void OnViewIsDeleting(views::View* observed_view) override;

  // views::MouseWatcherListener:
  void MouseMovedOutOfHost() override;

 private:
  friend WebContentsUserData;
  class CardZone;

  // |sidebar| hosts |contents|, and outlives it.
  SidebarHoverCard(content::WebContents* contents, views::View* sidebar);

  void OnPreviewImageAvailable(TabHoverCardThumbnailObserver* observer,
                               gfx::ImageSkia preview);

  // The screenshot part of Show(), also when a plugin had nothing to show.
  void ShowThumbnail(const tabs::TabData& data, bool show_preview);
  // Puts |view| (a plugin's preview, or null for none) in the card.
  void SetPluginPreview(std::unique_ptr<TabPreviewView> view);
  // A plugin's preview with its rows wired to |preview_click_|.
  std::unique_ptr<TabPreviewView> MakePluginPreview(const TabPreview& preview);
  void OnPreviewItemClicked(const LivePreviewItem& item, int flags);
  // The row, the card, and the gap between them, in screen coordinates.
  bool IsInCardZone(const gfx::Point& screen_point) const;
  void OnPluginPreview(int request,
                       tabs::TabData data,
                       bool show_preview,
                       std::optional<TabPreview> preview);

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
  // Whether a plugin's preview (or its loading row) is in the card.
  bool has_plugin_preview_ = false;
  // Whether that preview's rows take clicks: Release() keeps the card.
  bool has_clickable_preview_ = false;
  // The current Show()'s, for the preview's rows.
  TabPreviewView::ItemCallback preview_click_;
  // After Release(), until the pointer leaves the card's zone.
  std::unique_ptr<views::MouseWatcher> mouse_watcher_;
  // The latest Show() that asked a plugin: older answers are dropped.
  int plugin_request_ = 0;
  base::WeakPtrFactory<SidebarHoverCard> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_HOVER_CARD_H_
