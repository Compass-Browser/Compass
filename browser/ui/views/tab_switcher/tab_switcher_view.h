// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_TAB_SWITCHER_VIEW_H_
#define COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_TAB_SWITCHER_VIEW_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/views/view.h"

namespace views {
class BubbleDialogDelegate;
class Widget;
}  // namespace views

class ThumbnailImage;

namespace compass {

class TabSwitcherCard;
class TabSwitcherView;

// The cards' bubble, owned by the switcher: closing it is destroying it.
struct TabSwitcherBubble {
  TabSwitcherBubble();
  TabSwitcherBubble(const TabSwitcherBubble&) = delete;
  TabSwitcherBubble& operator=(const TabSwitcherBubble&) = delete;
  ~TabSwitcherBubble();

  // Outlives |widget| (declared first, destroyed last).
  std::unique_ptr<views::BubbleDialogDelegate> delegate;
  std::unique_ptr<views::Widget> widget;
  raw_ptr<TabSwitcherView> view = nullptr;
};

// The recent tabs switcher's row of cards (tab_switcher.h): each entry's
// screenshot (a live tab's), favicon and title, the picked one highlighted,
// centered over the window. It never takes the focus, so the keys keep
// going where they went.
class TabSwitcherView : public views::View {
  METADATA_HEADER(TabSwitcherView, views::View)

 public:
  struct Card {
    Card();
    Card(const Card&);
    Card& operator=(const Card&);
    ~Card();

    std::u16string title;
    ui::ImageModel favicon;
    // A live tab's screenshot, or null.
    scoped_refptr<ThumbnailImage> thumbnail;
  };

  // Shows the cards over |parent|'s window; a click on one runs |picked|.
  // |closed| runs if the bubble closes by itself (with its window), and
  // must destroy it.
  static std::unique_ptr<TabSwitcherBubble> Show(
      views::Widget* parent,
      std::vector<Card> cards,
      size_t selected,
      base::RepeatingCallback<void(size_t)> picked,
      base::OnceClosure closed);

  TabSwitcherView(const TabSwitcherView&) = delete;
  TabSwitcherView& operator=(const TabSwitcherView&) = delete;
  ~TabSwitcherView() override;

  void SetSelected(size_t index);

 private:
  // |max_width|: the row's, within the window.
  TabSwitcherView(std::vector<Card> cards,
                  size_t selected,
                  int max_width,
                  base::RepeatingCallback<void(size_t)> picked);

  std::vector<raw_ptr<TabSwitcherCard>> cards_;
  // How many cards show at a time, and the first that does: a row too long
  // for the window slides to keep the picked one in view.
  size_t visible_ = 0;
  size_t first_ = 0;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_TAB_SWITCHER_VIEW_H_
