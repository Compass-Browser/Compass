// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/frame/space_fade.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_tree_owner.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

// |layer| and its shown descendants. A surface layer's clone shows the same
// surface, so a page's copy keeps its last frame, which viz keeps while the
// copy refers to it; painted layers come out empty.
std::unique_ptr<ui::Layer> CloneTree(const ui::Layer& layer) {
  std::unique_ptr<ui::Layer> clone = layer.Clone();
  for (ui::Layer* child : layer.children()) {
    if (child->GetTargetVisibility()) {
      clone->Add(CloneTree(*child).release());
    }
  }
  return clone;
}

}  // namespace

SpaceFade::SpaceFade(views::View* browser_view,
                     BrowserWindowInterface* browser)
    : browser_view_(browser_view), browser_(browser) {
  if (WindowFeatures* features = WindowFeatures::From(browser)) {
    features_ = features;
    features_observation_.Observe(features);
  }
}

SpaceFade::~SpaceFade() = default;

std::vector<ContentsWebView*> SpaceFade::GetPanes() const {
  std::vector<ContentsWebView*> panes;
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser_);
  MultiContentsView* multi =
      browser_view ? browser_view->multi_contents_view() : nullptr;
  if (!multi) {
    return panes;
  }
  for (ContentsContainerView* container : multi->contents_container_views()) {
    ContentsWebView* pane = container ? container->contents_view() : nullptr;
    if (pane && pane->layer()) {
      panes.push_back(pane);
    }
  }
  return panes;
}

void SpaceFade::OnSpacePreviewChanged() {
  // The new space's pages are fading in: a late preview from the page's
  // slide doesn't touch them.
  if (copy_ || !features_ || !gfx::Animation::ShouldRenderRichAnimation()) {
    return;
  }
  // The pages fade with the fingers, into the frame's color, which blends
  // toward the space pulled in (SidebarTint).
  if (!features_->preview_space()) {
    // Let go without switching: back, as the tint fades back.
    for (ContentsWebView* pane : GetPanes()) {
      ui::ScopedLayerAnimationSettings settings(pane->layer()->GetAnimator());
      settings.SetTransitionDuration(kDuration);
      settings.SetTweenType(gfx::Tween::EASE_OUT);
      pane->layer()->SetOpacity(1.f);
    }
    return;
  }
  const float opacity =
      1.f - static_cast<float>(features_->preview_progress());
  for (ContentsWebView* pane : GetPanes()) {
    pane->layer()->GetAnimator()->StopAnimating();
    pane->layer()->SetOpacity(opacity);
  }
}

void SpaceFade::OnSpaceWillSwitch() {
  // A fade still going: its copy goes, the pages it covered being the ones
  // copied now.
  clear_timer_.Stop();
  copy_.reset();
  if (!gfx::Animation::ShouldRenderRichAnimation()) {
    return;
  }
  views::Widget* widget = browser_view_->GetWidget();
  if (!widget || !widget->IsVisible() || !widget->GetLayer()) {
    return;
  }
  std::unique_ptr<ui::Layer> root = ui::Layer::Create(ui::LAYER_NOT_DRAWN);
  root->SetName("SpaceFade");
  root->SetBounds(widget->GetLayer()->bounds());
  for (ContentsWebView* pane : GetPanes()) {
    if (!pane->IsDrawn()) {
      continue;
    }
    // In the window's coordinates, which are the root layer's. As faded as
    // a swipe left it.
    std::unique_ptr<ui::Layer> copy = CloneTree(*pane->layer());
    copy->SetTransform(gfx::Transform());
    copy->SetBounds(pane->ConvertRectToWidget(pane->GetLocalBounds()));
    root->Add(copy.release());
  }
  if (root->children().empty()) {
    return;
  }
  widget->GetLayer()->Add(root.get());
  widget->GetLayer()->StackAtTop(root.get());
  copy_ = std::make_unique<ui::LayerTreeOwner>(std::move(root));
}

void SpaceFade::OnSpaceSwitched() {
  if (!copy_) {
    for (ContentsWebView* pane : GetPanes()) {
      pane->layer()->SetOpacity(1.f);
    }
    return;
  }
  ui::Layer* root = copy_->root();
  // Views reorder their layers as the panes change: the copy stays on top.
  if (root->parent()) {
    root->parent()->StackAtTop(root);
  }
  // The old pages fade out, slowly at first, while the new ones fade in,
  // quickly at first: together they hardly let the frame show through, and
  // the new pages have a moment to draw their first frame.
  {
    ui::ScopedLayerAnimationSettings settings(root->GetAnimator());
    settings.SetTransitionDuration(kDuration);
    settings.SetTweenType(gfx::Tween::EASE_IN);
    settings.SetPreemptionStrategy(
        ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
    root->SetOpacity(0.f);
  }
  for (ContentsWebView* pane : GetPanes()) {
    ui::Layer* layer = pane->layer();
    layer->GetAnimator()->StopAnimating();
    layer->SetOpacity(0.f);
    ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
    settings.SetTransitionDuration(kDuration);
    settings.SetTweenType(gfx::Tween::EASE_OUT);
    layer->SetOpacity(1.f);
  }
  clear_timer_.Start(FROM_HERE, kDuration,
                     base::BindOnce(&SpaceFade::Clear, base::Unretained(this)));
}

void SpaceFade::Clear() {
  clear_timer_.Stop();
  copy_.reset();
  for (ContentsWebView* pane : GetPanes()) {
    pane->layer()->GetAnimator()->StopAnimating();
    pane->layer()->SetOpacity(1.f);
  }
}

}  // namespace compass
