// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/page_info_look/page_info_look.h"

#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "compass/browser/ui/views/bubble_look.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/color/color_variant.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/separator.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_observer.h"
#include "ui/views/view_utils.h"

namespace compass {

namespace {

// The downloads bubble's (downloads_library_view.cc): cards 12 pixels in
// from the bubble's edges, 12-pixel corners.
constexpr int kCardInset = 12;
constexpr int kCardRadius = 12;
constexpr int kCardGap = 8;
// Room above and below a card's rows, so their hover backgrounds don't touch
// its corners.
constexpr int kCardPadding = 4;

// Restyles the bubble's pages as they come, and their sections as their
// rows come and go. Deletes itself with the bubble.
class PageInfoLook : public views::ViewObserver {
 public:
  PageInfoLook(views::BubbleDialogDelegateView* bubble,
               views::View* pages,
               std::optional<SkColor> tint)
      : bubble_(bubble), pages_(pages), tint_(tint) {
    observation_.AddObservation(bubble_);
    observation_.AddObservation(pages_);
    Restyle();
  }
  PageInfoLook(const PageInfoLook&) = delete;
  PageInfoLook& operator=(const PageInfoLook&) = delete;
  ~PageInfoLook() override = default;

  // views::ViewObserver:
  void OnViewThemeChanged(views::View* view) override {
    if (view == bubble_) {
      Restyle();
    }
  }

  void OnChildViewAdded(views::View* observed, views::View* child) override {
    if (observed == pages_) {
      // A page switched to: its sections are added before it is.
      StylePage(child);
    } else {
      StyleSection(observed);
    }
  }

  void OnChildViewRemoved(views::View* observed, views::View* child) override {
    if (observed != pages_ && observed != bubble_) {
      StyleSection(observed);
    }
  }

  void OnViewIsDeleting(views::View* view) override {
    if (view == bubble_) {
      delete this;
      return;
    }
    observation_.RemoveObservation(view);
    if (view == pages_) {
      pages_ = nullptr;
    }
  }

 private:
  // Everything, as after a change of theme.
  void Restyle() {
    if (!bubble_ || !bubble_->GetColorProvider()) {
      return;
    }
    look_ = ComputeBubbleLook(bubble_->GetColorProvider(), tint_);
    bubble_->SetBackgroundColor(ui::ColorVariant(look_->background));
    if (pages_) {
      for (views::View* page : pages_->children()) {
        StylePage(page);
      }
    }
  }

  // A page is its header, then its sections: the main page's (the
  // connection, the permissions, the site's settings and switches, Site
  // settings), or a sub-page's content.
  void StylePage(views::View* page) {
    const auto& children = page->children();
    for (size_t i = 1; i < children.size(); ++i) {
      if (!observation_.IsObservingSource(children[i])) {
        observation_.AddObservation(children[i]);
      }
      StyleSection(children[i]);
    }
  }

  void StyleSection(views::View* section) {
    if (!look_) {
      return;
    }
    // Upstream's separators between sections are the gaps between cards
    // now; those deeper inside a section stay.
    bool has_rows = false;
    for (views::View* child : section->children()) {
      if (views::IsViewClass<views::Separator>(child)) {
        child->SetVisible(false);
      } else {
        has_rows |= child->GetVisible();
      }
    }
    StyleDescendants(section);

    if (!has_rows) {
      // Nothing yet, only separators, or rows not shown yet: no card.
      ClearCard(section);
      return;
    }
    section->SetProperty(views::kMarginsKey,
                         gfx::Insets::TLBR(0, kCardInset, kCardGap, kCardInset));
    // A card of one button (Site settings) has the button's own padding.
    if (!views::IsViewClass<views::Button>(section)) {
      section->SetBorder(
          views::CreateEmptyBorder(gfx::Insets::VH(kCardPadding, 0)));
    }
    section->SetBackground(views::CreateRoundedRectBackground(
        ui::ColorVariant(look_->card), kCardRadius));
    // Its rows' hover backgrounds stay inside its corners.
    if (!section->layer()) {
      section->SetPaintToLayer();
      section->layer()->SetFillsBoundsOpaquely(false);
    }
    section->layer()->SetRoundedCornerRadius(
        gfx::RoundedCornersF(kCardRadius));
    section->layer()->SetMasksToBounds(true);
    section->InvalidateLayout();
  }

  void ClearCard(views::View* section) {
    section->ClearProperty(views::kMarginsKey);
    if (!views::IsViewClass<views::Button>(section)) {
      section->SetBorder(nullptr);
    }
    section->SetBackground(nullptr);
  }

  void StyleDescendants(views::View* view) {
    for (views::View* child : view->children()) {
      if (auto* scroll = views::AsViewClass<views::ScrollView>(child)) {
        // Scrolling contents paint on an opaque layer: the card's color.
        scroll->SetBackgroundColor(ui::ColorVariant(look_->card));
      }
      StyleDescendants(child);
    }
  }

  raw_ptr<views::BubbleDialogDelegateView> bubble_;
  raw_ptr<views::View> pages_;
  const std::optional<SkColor> tint_;
  std::optional<BubbleLook> look_;
  base::ScopedMultiSourceObservation<views::View, views::ViewObserver>
      observation_{this};
};

}  // namespace

void InstallPageInfoLook(views::BubbleDialogDelegateView* bubble,
                         views::View* pages,
                         content::WebContents* contents) {
  BrowserWindowInterface* browser = nullptr;
  if (tabs::TabInterface* tab =
          contents ? tabs::TabInterface::MaybeGetFromContents(contents)
                   : nullptr) {
    browser = tab->GetBrowserWindowInterface();
  }
  // Deletes itself with the bubble.
  new PageInfoLook(bubble, pages, GetBubbleTint(browser));
}

}  // namespace compass
