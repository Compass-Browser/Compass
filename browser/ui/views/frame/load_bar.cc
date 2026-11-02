// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/load_bar.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "base/functional/bind.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "compass/browser/ui/views/frame/content_frame.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/view_class_properties.h"

namespace compass {

namespace {

constexpr int kHeight = 2;
constexpr base::TimeDelta kFrameInterval = base::Hertz(60);
// One sweep across, while connecting.
constexpr double kSweepSeconds = 1.2;
// The sweeping segment's share of the line.
constexpr double kSweepWidth = 0.3;
// How fast the line catches up with the load's progress, per second.
constexpr double kCatchUpRate = 8;
constexpr double kFadeSeconds = 0.25;
// A load's line starts with a sliver, so it shows at once.
constexpr double kMinProgress = 0.05;

}  // namespace

// static
LoadBar* LoadBar::Install(views::View* browser_view,
                          BrowserWindowInterface* browser) {
  auto* bar = browser_view->AddChildView(std::make_unique<LoadBar>(browser));
  bar->host_observation_.Observe(browser_view);
  return bar;
}

LoadBar::LoadBar(BrowserWindowInterface* browser) : browser_(browser) {
  SetProperty(views::kViewIgnoredByLayoutKey, true);
  SetCanProcessEventsWithinSubtree(false);
  GetViewAccessibility().SetIsIgnored(true);
  // Over the browser view's other layers.
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetVisible(false);
  active_tab_subscription_ = browser->RegisterActiveTabDidChange(
      base::BindRepeating(&LoadBar::OnActiveTabChanged,
                          base::Unretained(this)));
  OnActiveTabChanged(browser);
}

LoadBar::~LoadBar() = default;

void LoadBar::OnActiveTabChanged(BrowserWindowInterface* browser) {
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  Observe(tab ? tab->GetContents() : nullptr);
  // Another tab's load isn't this one's: start over.
  shown_ = false;
  finishing_ = false;
  timer_.Stop();
  SetVisible(false);
  Update();
}

void LoadBar::DidStartLoading() {
  Update();
}

void LoadBar::DidStopLoading() {
  Update();
}

void LoadBar::LoadProgressChanged(double progress) {
  Update();
}

void LoadBar::Update() {
  content::WebContents* contents = web_contents();
  const bool loading = contents && contents->ShouldShowLoadingUI();
  if (loading) {
    waiting_ = contents->IsWaitingForResponse();
    if (!shown_ || finishing_) {
      // A new load (or one that came back while the last faded).
      if (!shown_) {
        progress_ = kMinProgress;
        sweep_time_ = 0;
      }
      shown_ = true;
      finishing_ = false;
      opacity_ = 1;
      UpdateBounds();
      SetVisible(true);
      last_tick_ = base::TimeTicks::Now();
      timer_.Start(FROM_HERE, kFrameInterval,
                   base::BindRepeating(&LoadBar::Tick,
                                       base::Unretained(this)));
    }
    SchedulePaint();
    return;
  }
  if (shown_ && !finishing_) {
    finishing_ = true;
    waiting_ = false;
  }
}

void LoadBar::Tick() {
  const base::TimeTicks now = base::TimeTicks::Now();
  const double dt = std::min((now - last_tick_).InSecondsF(), 0.1);
  last_tick_ = now;
  content::WebContents* contents = web_contents();
  if (!finishing_ && contents) {
    waiting_ = contents->IsWaitingForResponse();
  }
  const double target =
      finishing_ ? 1.0
                 : std::max(contents ? contents->GetLoadProgress() : 0.0,
                            kMinProgress);
  // Eases toward the target; never back.
  progress_ = std::max(
      progress_, progress_ + (target - progress_) *
                                 std::min(1.0, dt * kCatchUpRate));
  if (waiting_) {
    sweep_time_ += dt;
  }
  if (finishing_ && progress_ > 0.99) {
    opacity_ -= dt / kFadeSeconds;
    if (opacity_ <= 0) {
      shown_ = false;
      finishing_ = false;
      timer_.Stop();
      SetVisible(false);
      return;
    }
  }
  UpdateBounds();
  SchedulePaint();
}

void LoadBar::UpdateBounds() {
  auto* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  views::View* card =
      browser_view ? browser_view->multi_contents_view() : nullptr;
  if (!card || !parent() || !card->GetVisible()) {
    SetBoundsRect(gfx::Rect());
    return;
  }
  gfx::Rect bounds =
      views::View::ConvertRectToTarget(card, parent(), card->GetLocalBounds());
  // Centered in the frame's gap above the card; none when the card is at
  // the window's top (fullscreen).
  if (bounds.y() < kContentFrameInset) {
    SetBoundsRect(gfx::Rect());
    return;
  }
  const int corner = static_cast<int>(kContentCornerRadius);
  bounds.Inset(gfx::Insets::VH(0, corner));
  SetBoundsRect(gfx::Rect(bounds.x(),
                          bounds.y() - (kContentFrameInset + kHeight) / 2,
                          bounds.width(), kHeight));
}

void LoadBar::OnPaint(gfx::Canvas* canvas) {
  if (!shown_ || width() == 0) {
    return;
  }
  // The sidebar's text color: readable on any space's tint.
  SkColor color = GetColorProvider()->GetColor(ui::kColorSysOnSurface);
  color = SkColorSetA(
      color, static_cast<U8CPU>(SkColorGetA(color) * 0.7 *
                                std::clamp(opacity_, 0.0, 1.0)));
  const float w = width();
  gfx::RectF line;
  if (waiting_) {
    const double phase =
        (1 - std::cos(sweep_time_ / kSweepSeconds * std::numbers::pi)) / 2;
    const float segment = w * kSweepWidth;
    line = gfx::RectF((w - segment) * phase, 0, segment, kHeight);
  } else {
    line = gfx::RectF(0, 0, w * std::clamp(progress_, 0.0, 1.0), kHeight);
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setColor(color);
  canvas->DrawRoundRect(line, kHeight / 2.f, flags);
}

void LoadBar::OnChildViewAdded(views::View* observed_view,
                               views::View* child) {
  // Stays over what the browser view adds later.
  if (child != this) {
    observed_view->ReorderChildView(this, observed_view->children().size());
  }
}

void LoadBar::OnViewIsDeleting(views::View* observed_view) {
  host_observation_.Reset();
}

BEGIN_METADATA(LoadBar)
END_METADATA

}  // namespace compass
