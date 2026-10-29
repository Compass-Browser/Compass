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
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "strata/browser/ui/views/frame/browser_view_exchange.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "strata/browser/window_sync/window_features.h"
#include "ui/base/base_window.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_lock.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace strata {

namespace {

// Long enough for a page loading from the network to show something; a swap
// never waits longer.
constexpr base::TimeDelta kMaxSwapWait = base::Milliseconds(750);
// A sidebar built hidden takes its tree in ~0.5 s in an optimized build, and
// the window doesn't change until it has.
constexpr base::TimeDelta kMaxTreeWait = base::Seconds(2);
base::TimeDelta g_max_tree_wait = kMaxTreeWait;

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

// Spike (docs/design/profiles-in-place.md): the new profile's window, built
// hidden, gives its view to the shown native window. Its sidebar takes the
// tree first, and the shown window keeps its last frame until the sidebar and
// the page have painted in it. Owns itself.
class ExchangeTransition : public content::WebContentsObserver,
                           public ui::CompositorLockClient {
 public:
  ExchangeTransition(BrowserWindowInterface* old,
                     BrowserWindowInterface* window,
                     base::OnceClosure leave)
      : old_(old->GetWeakPtr()),
        window_(window->GetWeakPtr()),
        leave_(std::move(leave)) {}
  ExchangeTransition(const ExchangeTransition&) = delete;
  ExchangeTransition& operator=(const ExchangeTransition&) = delete;
  ~ExchangeTransition() override = default;

  void Start() {
    timeout_.Start(FROM_HERE, g_max_tree_wait,
                   base::BindOnce(&ExchangeTransition::Exchange,
                                  base::Unretained(this)));
    BrowserView* view = BrowserView::GetBrowserViewForBrowser(window_.get());
    StrataSidebarView* sidebar = FindSidebar(view);
    if (!sidebar || !sidebar->GetWebContents()) {
      Exchange();
      return;
    }
    Observe(sidebar->GetWebContents());
    WaitForTree();
  }

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override { WaitForTree(); }
  void WebContentsDestroyed() override { Exchange(); }

  // ui::CompositorLockClient:
  void CompositorLockTimedOut() override { Unlock(); }

 private:
  // Calls back once |contents| has painted: its first paint, or a frame with
  // what it shows now.
  class PaintWaiter : public content::WebContentsObserver {
   public:
    PaintWaiter(content::WebContents* contents, base::OnceClosure painted)
        : painted_(std::move(painted)) {
      if (!contents->CompletedFirstVisuallyNonEmptyPaint()) {
        Observe(contents);
        return;
      }
      contents->GetPrimaryMainFrame()->InsertVisualStateCallback(
          base::BindOnce(
              [](base::WeakPtr<PaintWaiter> waiter, bool) {
                if (waiter) {
                  waiter->Painted();
                }
              },
              weak_factory_.GetWeakPtr()));
    }

    void DidFirstVisuallyNonEmptyPaint() override { Painted(); }
    void WebContentsDestroyed() override { Painted(); }

    bool waiting() const { return !painted_.is_null(); }

   private:
    void Painted() {
      Observe(nullptr);
      if (painted_) {
        std::move(painted_).Run();
      }
    }

    base::OnceClosure painted_;
    base::WeakPtrFactory<PaintWaiter> weak_factory_{this};
  };

  void WaitForTree() {
    content::WebUI* web_ui = web_contents()->GetWebUI();
    StrataSidebarUI* ui =
        web_ui ? web_ui->GetController()->GetAs<StrataSidebarUI>() : nullptr;
    if (!ui) {
      return;
    }
    ui->RunWhenTreeShown(base::BindOnce(&ExchangeTransition::Exchange,
                                        weak_factory_.GetWeakPtr()));
  }

  void Exchange() {
    if (exchanged_) {
      return;
    }
    exchanged_ = true;
    timeout_.Stop();
    Observe(nullptr);
    BrowserView* old_view =
        old_ ? BrowserView::GetBrowserViewForBrowser(old_.get()) : nullptr;
    BrowserView* view =
        window_ ? BrowserView::GetBrowserViewForBrowser(window_.get())
                : nullptr;
    if (!old_view || !view) {
      Finish();
      return;
    }
    BrowserViewExchange::Exchange(*old_view, *view);

    if (ui::Compositor* compositor = view->GetWidget()->GetCompositor()) {
      lock_ = compositor->GetCompositorLock(this, kMaxSwapWait);
    }
    StrataSidebarView* sidebar = FindSidebar(view);
    tabs::TabInterface* tab = window_->GetActiveTabInterface();
    for (content::WebContents* contents :
         {sidebar ? sidebar->GetWebContents() : nullptr,
          tab ? tab->GetContents() : nullptr}) {
      if (contents) {
        waiters_.push_back(std::make_unique<PaintWaiter>(
            contents, base::BindOnce(&ExchangeTransition::MaybeUnlock,
                                     base::Unretained(this))));
      }
    }
    MaybeUnlock();
  }

  void MaybeUnlock() {
    for (const auto& waiter : waiters_) {
      if (waiter->waiting()) {
        return;
      }
    }
    Unlock();
  }

  void Unlock() {
    if (unlocked_) {
      return;
    }
    unlocked_ = true;
    lock_.reset();
    Finish();
  }

  void Finish() {
    std::move(leave_).Run();
    delete this;
  }

  base::WeakPtr<BrowserWindowInterface> old_;
  base::WeakPtr<BrowserWindowInterface> window_;
  base::OnceClosure leave_;
  base::OneShotTimer timeout_;
  bool exchanged_ = false;
  bool unlocked_ = false;
  std::unique_ptr<ui::CompositorLock> lock_;
  std::vector<std::unique_ptr<PaintWaiter>> waiters_;
  base::WeakPtrFactory<ExchangeTransition> weak_factory_{this};
};

}  // namespace

void SetMaxTreeWaitForTesting(base::TimeDelta wait) {
  g_max_tree_wait = wait;
}

void InstallSwapPresenter() {
  if (g_installed) {
    return;
  }
  g_installed = true;
  SetSwapPresenter(base::BindRepeating([](BrowserWindowInterface* old,
                                          BrowserWindowInterface* window,
                                          base::OnceClosure leave) {
    if (BrowserView::GetBrowserViewForBrowser(old) &&
        BrowserView::GetBrowserViewForBrowser(window)) {
      (new ExchangeTransition(old, window, std::move(leave)))->Start();
      return;
    }
    (new SwapTransition(old, window, std::move(leave)))->Start();
  }));
}

}  // namespace strata
