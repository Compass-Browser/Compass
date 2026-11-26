// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/tab_preview_view.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "compass/browser/ui/live_folders/tab_preview_service.h"
#include "compass/strings/grit/compass_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/time_format.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/box_layout_view.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/style/typography.h"

namespace compass {

namespace {

// Text lines up with the card's title and domain (upstream's kTextMargins,
// 12): the view's margin plus each child's own inset, which is where a row's
// hover highlight reaches.
constexpr int kSideMargin = 6;
constexpr int kChildInset = 6;
// Half the space between rows: a row's highlight takes its share.
constexpr int kHalfRowSpacing = 3;
constexpr int kRowRadius = 6;
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

// The header, the footer and the loading line, inset as the rows are.
std::unique_ptr<views::Label> MakeLine(const std::u16string& text,
                                       int style) {
  auto label = MakeLabel(text, style, true);
  label->SetBorder(
      views::CreateEmptyBorder(gfx::Insets::VH(kHalfRowSpacing, kChildInset)));
  return label;
}

std::unique_ptr<views::View> MakeRowContent(const LivePreviewItem& item,
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
  row->SetBorder(
      views::CreateEmptyBorder(gfx::Insets::VH(kHalfRowSpacing, kChildInset)));
  return row;
}

// A row that takes clicks (left or middle button), lit while the pointer is
// on it.
class PreviewRowButton : public views::Button {
  METADATA_HEADER(PreviewRowButton, views::Button)

 public:
  PreviewRowButton(std::unique_ptr<views::View> content,
                   const std::u16string& name,
                   PressedCallback callback)
      : views::Button(std::move(callback)) {
    SetLayoutManager(std::make_unique<views::FillLayout>());
    // The button gets the events, not the labels.
    content->SetCanProcessEventsWithinSubtree(false);
    AddChildView(std::move(content));
    SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |
                             ui::EF_MIDDLE_MOUSE_BUTTON);
    // The card can't take focus.
    SetFocusBehavior(FocusBehavior::NEVER);
    GetViewAccessibility().SetName(name);
  }

  // views::Button:
  void StateChanged(ButtonState old_state) override {
    views::Button::StateChanged(old_state);
    const bool lit =
        GetState() == STATE_HOVERED || GetState() == STATE_PRESSED;
    SetBackground(lit ? views::CreateRoundedRectBackground(
                            ui::kColorSysStateHoverOnSubtle, kRowRadius)
                      : nullptr);
  }
};

BEGIN_METADATA(PreviewRowButton)
END_METADATA

void SetUpLayout(views::View* view) {
  auto* layout = view->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical,
      gfx::Insets::TLBR(8 - kHalfRowSpacing, kSideMargin,
                        10 - kHalfRowSpacing, kSideMargin)));
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);
}

}  // namespace

TabPreviewView::TabPreviewView(const TabPreview& preview,
                               base::Time now,
                               ItemCallback on_click) {
  SetUpLayout(this);
  const LivePreviewResult& result = preview.result;
  if (!result.title.empty()) {
    AddChildView(MakeLine(result.title, views::style::STYLE_BODY_5_MEDIUM));
  }
  for (const LivePreviewItem& item : result.items) {
    std::unique_ptr<views::View> row = MakeRowContent(item, now);
    if (!on_click) {
      AddChildView(std::move(row));
      continue;
    }
    AddChildView(std::make_unique<PreviewRowButton>(
        std::move(row), item.title,
        base::BindRepeating(
            [](const ItemCallback& on_click, const LivePreviewItem& item,
               const ui::Event& event) { on_click.Run(item, event.flags()); },
            on_click, item)));
    has_clickable_rows_ = true;
  }
  if (!result.footer.empty()) {
    AddChildView(MakeLine(result.footer, views::style::STYLE_BODY_5));
  }
}

TabPreviewView::TabPreviewView() : loading_(true) {
  SetUpLayout(this);
  AddChildView(
      MakeLine(l10n_util::GetStringUTF16(IDS_COMPASS_TAB_PREVIEW_LOADING),
               views::style::STYLE_BODY_5));
}

TabPreviewView::~TabPreviewView() = default;

BEGIN_METADATA(TabPreviewView)
END_METADATA

}  // namespace compass
