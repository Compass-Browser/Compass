// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/tab_preview_view.h"

#include <memory>
#include <string>

#include "chrome/browser/ui/color/chrome_color_id.h"
#include "compass/browser/ui/live_folders/tab_preview_service.h"
#include "ui/base/l10n/time_format.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/views/border.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/background.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/style/typography.h"

namespace compass {

namespace {

// As the card's title and domain (upstream's kTextMargins).
constexpr int kSideMargin = 12;
constexpr int kRowSpacing = 6;
constexpr int kBadgeRadius = 4;

std::unique_ptr<views::Label> MakeLabel(const std::u16string& text,
                                        int style,
                                        bool secondary) {
  auto label = std::make_unique<views::Label>(
      text, views::style::CONTEXT_DIALOG_BODY_TEXT, style);
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetElideBehavior(gfx::ELIDE_TAIL);
  label->SetMultiLine(false);
  if (secondary) {
    label->SetEnabledColor(kColorTabHoverCardSecondaryText);
  }
  return label;
}

std::unique_ptr<views::View> MakeRow(const LivePreviewItem& item,
                                     base::Time now) {
  auto row = std::make_unique<views::BoxLayoutView>();
  row->SetOrientation(views::BoxLayout::Orientation::kHorizontal);
  row->SetBetweenChildSpacing(6);
  row->SetCrossAxisAlignment(views::BoxLayout::CrossAxisAlignment::kStart);

  auto text = std::make_unique<views::BoxLayoutView>();
  text->SetOrientation(views::BoxLayout::Orientation::kVertical);
  text->AddChildView(
      MakeLabel(item.title, views::style::STYLE_BODY_4_MEDIUM, false));
  if (!item.subtitle.empty()) {
    text->AddChildView(
        MakeLabel(item.subtitle, views::style::STYLE_BODY_5, true));
  }
  views::View* text_view = row->AddChildView(std::move(text));
  row->SetFlexForView(text_view, 1);

  // How long ago, for things that happened (a calendar's coming events say
  // when in their subtitle).
  if (item.time && *item.time <= now) {
    row->AddChildView(MakeLabel(
        ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_ELAPSED,
                               ui::TimeFormat::LENGTH_SHORT, now - *item.time),
        views::style::STYLE_BODY_5, true));
  }
  if (!item.badge.empty()) {
    auto badge = MakeLabel(item.badge, views::style::STYLE_BODY_5_MEDIUM, true);
    badge->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(1, 5)));
    badge->SetBackground(views::CreateRoundedRectBackground(
        ui::kColorSysTonalContainer, kBadgeRadius));
    row->AddChildView(std::move(badge));
  }
  return row;
}

void SetUpLayout(views::View* view) {
  auto* layout = view->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical,
      gfx::Insets::TLBR(8, kSideMargin, 10, kSideMargin), kRowSpacing));
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);
}

}  // namespace

TabPreviewView::TabPreviewView(const TabPreview& preview, base::Time now) {
  SetUpLayout(this);
  const LivePreviewResult& result = preview.result;
  if (!result.title.empty()) {
    AddChildView(MakeLabel(result.title, views::style::STYLE_BODY_5_MEDIUM,
                           true));
  }
  for (const LivePreviewItem& item : result.items) {
    AddChildView(MakeRow(item, now));
  }
  if (!result.footer.empty()) {
    AddChildView(MakeLabel(result.footer, views::style::STYLE_BODY_5, true));
  }
}

TabPreviewView::TabPreviewView() : loading_(true) {
  SetUpLayout(this);
  AddChildView(MakeLabel(u"Loading…", views::style::STYLE_BODY_5, true));
}

TabPreviewView::~TabPreviewView() = default;

BEGIN_METADATA(TabPreviewView)
END_METADATA

}  // namespace compass
