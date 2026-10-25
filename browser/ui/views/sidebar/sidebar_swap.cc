// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_swap.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/tabs/public/tab_interface.h"
#include "components/viz/common/frame_timing_details.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/window_sync/window_features.h"
#include "ui/base/base_window.h"
#include "ui/compositor/compositor.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace strata {

namespace {

// Long enough for a page loading from the network to show something; a swap
// never waits longer.
constexpr base::TimeDelta kMaxSwapWait = base::Milliseconds(750);

bool g_installed = false;

StrataSidebarView* FindSidebar(views::View* view) {
  if (auto* sidebar = views::AsViewClass<StrataSidebarView>(view)) {
    return sidebar;
  }
  for (views::View* child : view->children()) {
    if (StrataSidebarView* sidebar = FindSidebar(child)) {
      return sidebar;
    }
  }
  return nullptr;
}

views::Widget* GetWidget(BrowserWindowInterface* browser) {
  BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser);
  return view ? view->GetWidget() : nullptr;
}

// One swap, from the new window's showing to the old one's hiding. Owns
// itself.
class SwapTransition {
 public:
  SwapTransition(BrowserWindowInterface* old,
                 BrowserWindowInterface* window,
                 base::OnceClosure leave)
      : old_(old->GetWeakPtr()),
        window_(window->GetWeakPtr()),
        leave_(std::move(leave)) {}
  SwapTransition(const SwapTransition&) = delete;
  SwapTransition& operator=(const SwapTransition&) = delete;
  ~SwapTransition() = default;

  void Start() {
    views::Widget* widget = GetWidget(window_.get());
    if (!widget) {
      window_->GetWindow()->Show();
      Finish();
      return;
    }
    // Invisible, but not occluded: it renders, and the old window shows
    // through it.
    widget->SetVisibilityChangedAnimationsEnabled(false);
    widget->SetOpacity(0.f);
    window_->GetWindow()->Show();

    timeout_.Start(FROM_HERE, kMaxSwapWait,
                   base::BindOnce(&SwapTransition::Reveal,
                                  base::Unretained(this)));
    BrowserView* view = BrowserView::GetBrowserViewForBrowser(window_.get());
    if (StrataSidebarView* sidebar = FindSidebar(view)) {
      Wait(sidebar->GetWebContents());
    }
    if (tabs::TabInterface* tab = window_->GetActiveTabInterface()) {
      Wait(tab->GetContents());
    }
    MaybeReveal();
  }

 private:
  // Waits for |contents|' first paint, unless it has painted already.
  class PaintWaiter : public content::WebContentsObserver {
   public:
    PaintWaiter(content::WebContents* contents, SwapTransition* transition)
        : content::WebContentsObserver(contents), transition_(transition) {}

    void DidFirstVisuallyNonEmptyPaint() override {
      Observe(nullptr);
      transition_->MaybeReveal();
    }
    void WebContentsDestroyed() override {
      Observe(nullptr);
      transition_->MaybeReveal();
    }

    bool waiting() const { return web_contents() != nullptr; }

   private:
    const raw_ptr<SwapTransition> transition_;
  };

  void Wait(content::WebContents* contents) {
    if (contents && !contents->CompletedFirstVisuallyNonEmptyPaint()) {
      waiters_.push_back(std::make_unique<PaintWaiter>(contents, this));
    }
  }

  void MaybeReveal() {
    for (const auto& waiter : waiters_) {
      if (waiter->waiting()) {
        return;
      }
    }
    // Then once that paint is on screen.
    views::Widget* widget = window_ ? GetWidget(window_.get()) : nullptr;
    ui::Compositor* compositor = widget ? widget->GetCompositor() : nullptr;
    if (!compositor || requested_frame_) {
      Reveal();
      return;
    }
    requested_frame_ = true;
    compositor->RequestSuccessfulPresentationTimeForNextFrame(base::BindOnce(
        [](base::WeakPtr<SwapTransition> transition,
           const viz::FrameTimingDetails&) {
          if (transition) {
            transition->Reveal();
          }
        },
        weak_factory_.GetWeakPtr()));
  }

  // The new window turns opaque as the old one hides.
  void Reveal() {
    if (revealed_) {
      return;
    }
    revealed_ = true;
    timeout_.Stop();
    waiters_.clear();
    if (views::Widget* widget = window_ ? GetWidget(window_.get()) : nullptr) {
      widget->SetOpacity(1.f);
      widget->SetVisibilityChangedAnimationsEnabled(true);
    }
    if (views::Widget* widget = old_ ? GetWidget(old_.get()) : nullptr) {
      widget->SetVisibilityChangedAnimationsEnabled(false);
      widget->Hide();
    }
    Finish();
  }

  void Finish() {
    std::move(leave_).Run();
    delete this;
  }

  base::WeakPtr<BrowserWindowInterface> old_;
  base::WeakPtr<BrowserWindowInterface> window_;
  base::OnceClosure leave_;
  std::vector<std::unique_ptr<PaintWaiter>> waiters_;
  base::OneShotTimer timeout_;
  bool requested_frame_ = false;
  bool revealed_ = false;
  base::WeakPtrFactory<SwapTransition> weak_factory_{this};
};

}  // namespace

void InstallSwapPresenter() {
  if (g_installed) {
    return;
  }
  g_installed = true;
  SetSwapPresenter(base::BindRepeating([](BrowserWindowInterface* old,
                                          BrowserWindowInterface* window,
                                          base::OnceClosure leave) {
    (new SwapTransition(old, window, std::move(leave)))->Start();
  }));
}

}  // namespace strata
