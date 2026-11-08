// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/picture_in_picture/pip_style.h"

#include <algorithm>
#include <array>
#include <memory>

#include "base/containers/span.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/views/overlay/back_to_tab_button.h"
#include "chrome/browser/ui/views/overlay/close_image_button.h"
#include "chrome/browser/ui/views/overlay/hang_up_button.h"
#include "chrome/browser/ui/views/overlay/minimize_button.h"
#include "chrome/browser/ui/views/overlay/overlay_window_live_caption_button.h"
#include "chrome/browser/ui/views/overlay/overlay_window_live_caption_dialog.h"
#include "chrome/browser/ui/views/overlay/playback_image_button.h"
#include "chrome/browser/ui/views/overlay/simple_overlay_window_image_button.h"
#include "chrome/browser/ui/views/overlay/toggle_camera_button.h"
#include "chrome/browser/ui/views/overlay/toggle_microphone_button.h"
#include "chrome/browser/ui/views/overlay/toggle_mute_button.h"
#include "chrome/browser/ui/views/overlay/video_overlay_window_views.h"
#include "chrome/grit/generated_resources.h"
#include "components/global_media_controls/public/views/media_progress_view.h"
#include "compass/browser/ui/views/picture_in_picture/pip_tint.h"
#include "content/public/browser/video_picture_in_picture_window_controller.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/outsets.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/label.h"
#include "ui/views/style/typography.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace compass {

namespace {

// The dim over the video while the controls show (upstream: a darker one,
// plus black gradients at the top and bottom).
constexpr SkColor kDimColor = SkColorSetA(SK_ColorBLACK, 0x33);
// A pill's fill: dark and translucent, with kTintShare of the space color
// mixed in; a faint light edge; the video behind it blurred.
constexpr SkColor kPillBaseColor = SkColorSetRGB(0x1C, 0x1C, 0x1E);
constexpr SkAlpha kPillAlpha = 0xB3;
// The middle pill sits over the picture itself: lighter.
constexpr SkAlpha kCenterPillAlpha = 0x66;
constexpr float kTintShare = 0.2f;
constexpr SkColor kPillEdgeColor = SkColorSetA(SK_ColorWHITE, 0x1F);
constexpr float kPillBlurSigma = 24;
// The bottom pill holds two rows: its corners stay this round.
constexpr int kBottomPillRadius = 14;
// Back to tab's icon (back_to_tab_button.cc patch), and the space between
// it and its title, and after the title.
constexpr int kBackToTabIconSize = 20;
constexpr int kBackToTabTitleGap = 4;
constexpr int kBackToTabTitleEnd = 12;

enum class Group { kTopLeading, kTopTrailing, kCenter, kBottom };

// The frosted shape under a group of controls. A child of the window's
// scrim, which fades with the controls; it takes no events.
class PipPill : public views::View {
  METADATA_HEADER(PipPill, views::View)

 public:
  explicit PipPill(Group group) : group_(group) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetBackgroundBlur(kPillBlurSigma);
    layer()->SetName("PipPill");
    SetCanProcessEventsWithinSubtree(false);
  }

  Group group() const { return group_; }

  int GetRadius() const {
    return group_ == Group::kBottom ? std::min(kBottomPillRadius, height() / 2)
                                    : height() / 2;
  }

  // views::View:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(GetRadius()));
    layer()->SetIsFastRoundedCorner(true);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    SkColor fill = kPillBaseColor;
    if (const views::Widget* widget = GetWidget();
        widget && widget->user_color_override()) {
      fill = color_utils::AlphaBlend(*widget->user_color_override(), fill,
                                     kTintShare);
    }
    const gfx::Rect bounds = GetLocalBounds();
    canvas->FillRect(bounds, SkColorSetA(fill, group_ == Group::kCenter
                                                   ? kCenterPillAlpha
                                                   : kPillAlpha));
    cc::PaintFlags edge;
    edge.setAntiAlias(true);
    edge.setStyle(cc::PaintFlags::kStroke_Style);
    edge.setStrokeWidth(1);
    edge.setColor(kPillEdgeColor);
    gfx::RectF edge_bounds(bounds);
    edge_bounds.Inset(0.5f);
    canvas->DrawRoundRect(edge_bounds, GetRadius() - 0.5f, edge);
  }

  void OnThemeChanged() override {
    views::View::OnThemeChanged();
    SchedulePaint();
  }

 private:
  const Group group_;
};

BEGIN_METADATA(PipPill)
END_METADATA

// Whether |view| shows: visible, and so is its parent below |container|
// (the playback or call controls' box).
bool IsShown(const views::View* view, const views::View* container) {
  if (!view || !view->GetVisible()) {
    return false;
  }
  for (const views::View* parent = view->parent();
       parent && parent != container; parent = parent->parent()) {
    if (!parent->GetVisible()) {
      return false;
    }
  }
  return true;
}

// |view|'s bounds in |container|, its ancestor.
gfx::Rect BoundsIn(const views::View* view, const views::View* container) {
  gfx::Rect bounds = view->bounds();
  for (const views::View* parent = view->parent();
       parent && parent != container; parent = parent->parent()) {
    bounds.Offset(parent->origin().OffsetFromOrigin());
  }
  return bounds;
}

// The union of the shown |views|' bounds in |container|.
gfx::Rect UnionOfShown(base::span<views::View* const> views,
                       const views::View* container) {
  gfx::Rect bounds;
  for (const views::View* view : views) {
    if (IsShown(view, container)) {
      bounds.Union(BoundsIn(view, container));
    }
  }
  return bounds;
}

// Space between a group's controls and its pill's edge. The middle one's
// round ends need more room at the sides.
gfx::Outsets GetPillOutsets(Group group) {
  switch (group) {
    case Group::kTopLeading:
    case Group::kTopTrailing:
      return gfx::Outsets(PipStyle::kPillPadding);
    case Group::kCenter:
      return gfx::Outsets::VH(3 * PipStyle::kPillPadding,
                             8 * PipStyle::kPillPadding);
    case Group::kBottom:
      return gfx::Outsets(2 * PipStyle::kPillPadding);
  }
}

}  // namespace

// static
void PipStyle::SetUp(VideoOverlayWindowViews* window) {
  // No site name: back to tab says where the video comes from. Its
  // contents, not the title view: upstream's fades show the views they fade
  // (OverlayControlsFadeAnimation), the title view and the scrims among them.
  window->favicon_view_->SetVisible(false);
  window->origin_->SetVisible(false);
  window->controls_top_scrim_view_->SetBackground(nullptr);
  window->controls_bottom_scrim_view_->SetBackground(nullptr);
  window->controls_scrim_view_->SetBackground(
      views::CreateSolidBackground(kDimColor));

  // Back to tab says so: its title right of the icon, inside the button, so
  // a click on the title goes back too.
  views::ImageButton* back = window->back_to_tab_button_;
  const int icon_area = back->height();
  const int icon_inset = (icon_area - kBackToTabIconSize) / 2;
  auto* title = back->AddChildView(std::make_unique<views::Label>(
      l10n_util::GetStringUTF16(IDS_PICTURE_IN_PICTURE_BACK_TO_TAB_CONTROL_TEXT),
      views::style::CONTEXT_LABEL, views::style::STYLE_BODY_4));
  title->SetEnabledColor(kColorPipWindowForeground);
  title->SetBackgroundColor(SK_ColorTRANSPARENT);
  title->SetAutoColorReadabilityEnabled(false);
  title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  title->SetCanProcessEventsWithinSubtree(false);
  const int title_x = icon_inset + kBackToTabIconSize + kBackToTabTitleGap;
  const int title_width = title->GetPreferredSize().width();
  title->SetBounds(title_x, 0, title_width, icon_area);
  back->SetSize({title_x + title_width + kBackToTabTitleEnd, icon_area});
  back->SetImageHorizontalAlignment(views::ImageButton::ALIGN_LEFT);
  back->SetBorder(
      views::CreateEmptyBorder(gfx::Insets::TLBR(0, icon_inset, 0, 0)));
  // Its hover and focus ring: the pill's shape, not a circle.
  views::InstallPillHighlightPathGenerator(back);
  for (Group group : {Group::kTopLeading, Group::kTopTrailing, Group::kCenter,
                      Group::kBottom}) {
    window->controls_scrim_view_->AddChildView(
        std::make_unique<PipPill>(group));
  }
}

// static
void PipStyle::Layout(VideoOverlayWindowViews* window) {
  // The space color, as the tab's space has now.
  window->SetUserColorOverride(GetPipTint(
      window->controller_ ? window->controller_->GetWebContents() : nullptr));

  views::View* const container = window->controls_container_view_;
  const gfx::Size size = container->size();
  if (size.IsEmpty()) {
    return;
  }
  const std::array<views::View*, 1> top_leading = {
      window->back_to_tab_button_.get()};
  const std::array<views::View*, 2> top_trailing = {
      window->minimize_button_.get(), window->close_controls_view_.get()};
  const std::array<views::View*, 6> center = {
      window->replay_10_seconds_button_.get(),
      window->play_pause_controls_view_.get(),
      window->forward_10_seconds_button_.get(),
      window->toggle_camera_button_.get(),
      window->hang_up_button_.get(),
      window->toggle_microphone_button_.get()};
  const std::array<views::View*, 7> bottom = {
      window->previous_track_controls_view_.get(),
      window->progress_view_.get(),
      window->next_track_controls_view_.get(),
      window->timestamp_.get(),
      window->live_status_.get(),
      window->live_caption_button_.get(),
      window->toggle_mute_button_.get()};

  // Top: back to tab leading, minimize and close trailing, each group on
  // its pill kMargin from the edges.
  const int top = kMargin + kPillPadding;
  views::View* close = window->close_controls_view_;
  close->SetPosition(
      {size.width() - kMargin - kPillPadding - close->width(), top});
  views::View* minimize = window->minimize_button_;
  minimize->SetPosition({close->x() - minimize->width(), top});
  // As views::View's: the buttons' own SetPosition() takes the window's size.
  static_cast<views::View*>(window->back_to_tab_button_.get())
      ->SetPosition({kMargin + kPillPadding, top});

  // Bottom: upstream's row, moved up (and in) so its pill stays kMargin
  // from the window's edges and clear of its rounded corners.
  const gfx::Rect bottom_row = UnionOfShown(bottom, container);
  if (!bottom_row.IsEmpty()) {
    gfx::Rect pill = bottom_row;
    pill.Outset(GetPillOutsets(Group::kBottom));
    const int dy = std::min(0, size.height() - kMargin - pill.bottom());
    if (dy) {
      for (views::View* view : bottom) {
        if (view) {
          view->SetY(view->y() + dy);
        }
      }
      window->live_caption_dialog_->SetY(window->live_caption_dialog_->y() +
                                         dy);
    }
  }

  // The pills, under their groups, in the scrim's coordinates (it's a pixel
  // larger than the window all around).
  views::View* const scrim = window->controls_scrim_view_;
  for (views::View* child : scrim->children()) {
    auto* pill = views::AsViewClass<PipPill>(child);
    if (!pill) {
      continue;
    }
    gfx::Rect bounds;
    switch (pill->group()) {
      case Group::kTopLeading:
        bounds = UnionOfShown(top_leading, container);
        break;
      case Group::kTopTrailing:
        bounds = UnionOfShown(top_trailing, container);
        break;
      case Group::kCenter:
        bounds = UnionOfShown(center, container);
        break;
      case Group::kBottom:
        bounds = UnionOfShown(bottom, container);
        break;
    }
    pill->SetVisible(!bounds.IsEmpty());
    if (bounds.IsEmpty()) {
      continue;
    }
    bounds.Outset(GetPillOutsets(pill->group()));
    bounds.Offset(-scrim->x(), -scrim->y());
    pill->SetBoundsRect(bounds);
  }
}

// static
PipStyle::PillBounds PipStyle::GetPillBoundsForTesting(
    VideoOverlayWindowViews* window) {
  PillBounds result;
  views::View* const scrim = window->controls_scrim_view_;
  for (views::View* child : scrim->children()) {
    auto* pill = views::AsViewClass<PipPill>(child);
    if (!pill || !pill->GetVisible()) {
      continue;
    }
    gfx::Rect bounds = pill->bounds();
    bounds.Offset(scrim->x(), scrim->y());
    switch (pill->group()) {
      case Group::kTopLeading:
        result.top_leading = bounds;
        break;
      case Group::kTopTrailing:
        result.top_trailing = bounds;
        break;
      case Group::kCenter:
        result.center = bounds;
        break;
      case Group::kBottom:
        result.bottom = bounds;
        break;
    }
  }
  return result;
}

}  // namespace compass
