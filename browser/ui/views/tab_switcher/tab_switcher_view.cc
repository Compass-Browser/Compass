// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/tab_switcher/tab_switcher_view.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "chrome/browser/ui/thumbnails/thumbnail_image.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/color/color_id.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/style/typography.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

// The screenshot's largest and smallest widths, at 16:10. Cards shrink to
// fit the window, down to the smallest; past that, fewer show at a time.
constexpr int kMaxThumbnailWidth = 200;
constexpr int kMinThumbnailWidth = 140;
// A card's insets and border around its screenshot, and the row's spacing.
constexpr int kCardPadding = 8;
constexpr int kCardSpacing = 8;
// The bubble's margins and frame, around the row.
constexpr int kBubbleMargin = 12;
constexpr int kBubbleFrame = 24;

gfx::Size ThumbnailSize(int width) {
  return gfx::Size(width, width * 10 / 16);
}
constexpr int kCardRadius = 10;
constexpr int kSelectedBorder = 2;
constexpr int kFaviconSize = 16;
constexpr int kPlaceholderFaviconSize = 32;

}  // namespace

// One entry: its screenshot (or its favicon, large, until there is one or if
// it has none) over its favicon and title.
class TabSwitcherCard : public views::Button {
  METADATA_HEADER(TabSwitcherCard, views::Button)

 public:
  TabSwitcherCard(const TabSwitcherView::Card& card,
                  gfx::Size thumbnail_size,
                  PressedCallback pressed)
      : views::Button(std::move(pressed)), thumbnail_size_(thumbnail_size) {
    SetAccessibleName(card.title);
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(kCardPadding), 8));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    preview_ = AddChildView(std::make_unique<views::ImageView>());
    preview_->SetPreferredSize(thumbnail_size_);
    preview_->SetBackground(views::CreateRoundedRectBackground(
        ui::kColorSysNeutralContainer, kCardRadius - 4));
    // The favicon, large, until the screenshot comes (a vector icon needs
    // the widget's colors, so it's sized rather than rasterized here).
    preview_->SetImage(card.favicon);
    preview_->SetImageSize(
        gfx::Size(kPlaceholderFaviconSize, kPlaceholderFaviconSize));

    auto row = std::make_unique<views::BoxLayoutView>();
    row->SetOrientation(views::BoxLayout::Orientation::kHorizontal);
    row->SetBetweenChildSpacing(6);
    row->SetCrossAxisAlignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    auto* favicon = row->AddChildView(std::make_unique<views::ImageView>());
    favicon->SetImage(card.favicon);
    favicon->SetImageSize(gfx::Size(kFaviconSize, kFaviconSize));
    auto* title = row->AddChildView(std::make_unique<views::Label>(
        card.title, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_BODY_4));
    title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title->SetElideBehavior(gfx::ELIDE_TAIL);
    row->SetFlexForView(title, 1);
    title->SetMaximumWidth(thumbnail_size_.width() - kFaviconSize - 6);
    AddChildView(std::move(row));

    if (card.thumbnail) {
      thumbnail_ = card.thumbnail;
      subscription_ = thumbnail_->Subscribe();
      subscription_->SetUncompressedImageCallback(base::BindRepeating(
          &TabSwitcherCard::OnThumbnail, base::Unretained(this)));
      thumbnail_->RequestThumbnailImage();
    }
    SetSelected(false);
    // A fixed size: asked for its size within the row's, the stretching
    // layout would take all of it, leaving the other cards none.
    SetPreferredSize(GetPreferredSize());
  }

  void SetSelected(bool selected) {
    SetBorder(selected ? views::CreateRoundedRectBorder(
                             kSelectedBorder, kCardRadius,
                             ui::kColorFocusableBorderFocused)
                       : views::CreateEmptyBorder(kSelectedBorder));
    SetBackground(selected ? views::CreateRoundedRectBackground(
                                 ui::kColorSysStateHoverOnSubtle, kCardRadius)
                           : nullptr);
  }

 private:
  void OnThumbnail(gfx::ImageSkia image) {
    if (image.isNull()) {
      return;
    }
    preview_->SetImage(ui::ImageModel::FromImageSkia(
        gfx::ImageSkiaOperations::CreateResizedImage(
            image, skia::ImageOperations::RESIZE_BEST, thumbnail_size_)));
    preview_->SetImageSize(thumbnail_size_);
  }

  const gfx::Size thumbnail_size_;
  raw_ptr<views::ImageView> preview_ = nullptr;
  scoped_refptr<ThumbnailImage> thumbnail_;
  std::unique_ptr<ThumbnailImage::Subscription> subscription_;
};

BEGIN_METADATA(TabSwitcherCard)
END_METADATA

TabSwitcherView::Card::Card() = default;
TabSwitcherView::Card::Card(const Card&) = default;
TabSwitcherView::Card& TabSwitcherView::Card::operator=(const Card&) = default;
TabSwitcherView::Card::~Card() = default;

TabSwitcherBubble::TabSwitcherBubble() = default;
TabSwitcherBubble::~TabSwitcherBubble() = default;

// static
std::unique_ptr<TabSwitcherBubble> TabSwitcherView::Show(
    views::Widget* parent,
    std::vector<Card> cards,
    size_t selected,
    base::RepeatingCallback<void(size_t)> picked,
    base::OnceClosure closed) {
  auto bubble = std::make_unique<TabSwitcherBubble>();
  bubble->delegate = std::make_unique<views::BubbleDialogDelegate>(
      nullptr, views::BubbleBorder::FLOAT);
  views::BubbleDialogDelegate* delegate = bubble->delegate.get();
  delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  delegate->SetShowCloseButton(false);
  // Never takes the focus: the keys keep going to the window.
  delegate->SetCanActivate(false);
  delegate->set_close_on_deactivate(false);
  delegate->set_parent_window(parent->GetNativeView());
  // Centered over the window.
  delegate->SetAnchorRect(parent->GetWindowBoundsInScreen());
  delegate->set_margins(gfx::Insets(kBubbleMargin));
  bubble->view = delegate->SetContentsView(base::WrapUnique(
      new TabSwitcherView(std::move(cards), selected,
                          parent->GetWindowBoundsInScreen().width() * 9 / 10 -
                              2 * (kBubbleMargin + kBubbleFrame),
                          std::move(picked))));
  bubble->widget = views::BubbleDialogDelegate::CreateBubble(
      delegate, base::BindOnce(
                    [](base::OnceClosure closed,
                       views::Widget::ClosedReason) { std::move(closed).Run(); },
                    std::move(closed)));
  bubble->widget->ShowInactive();
  return bubble;
}

TabSwitcherView::TabSwitcherView(std::vector<Card> cards,
                                 size_t selected,
                                 int max_width,
                                 base::RepeatingCallback<void(size_t)> picked) {
  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
      kCardSpacing));
  // A card is its screenshot plus padding and the selection's border.
  const int extra = 2 * (kCardPadding + kSelectedBorder);
  const int count = static_cast<int>(cards.size());
  const int fit =
      (max_width - (count - 1) * kCardSpacing) / std::max(count, 1) - extra;
  const int width = std::clamp(fit, kMinThumbnailWidth, kMaxThumbnailWidth);
  visible_ = std::clamp<size_t>(
      (max_width + kCardSpacing) / (width + extra + kCardSpacing), 1,
      cards.size());
  for (size_t i = 0; i < cards.size(); ++i) {
    cards_.push_back(AddChildView(std::make_unique<TabSwitcherCard>(
        cards[i], ThumbnailSize(width), base::BindRepeating(picked, i))));
  }
  SetSelected(selected);
}

TabSwitcherView::~TabSwitcherView() = default;

void TabSwitcherView::SetSelected(size_t index) {
  // Keep the picked card among the ones showing, sliding the row as little
  // as it takes.
  if (index < first_) {
    first_ = index;
  } else if (index >= first_ + visible_) {
    first_ = index + 1 - visible_;
  }
  for (size_t i = 0; i < cards_.size(); ++i) {
    cards_[i]->SetSelected(i == index);
    cards_[i]->SetVisible(i >= first_ && i < first_ + visible_);
  }
}

BEGIN_METADATA(TabSwitcherView)
END_METADATA

}  // namespace compass
