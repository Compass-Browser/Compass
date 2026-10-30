// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_swap.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "strata/browser/ui/views/frame/browser_view_exchange.h"
#include "strata/browser/ui/views/sidebar/sidebar_preloader.h"
#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "strata/browser/window_sync/window_features.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_lock.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace strata {

namespace {

// Long enough for a page loading from the network to show something; the
// window never holds its last frame longer.
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

// The new profile's window, built hidden, gives its view to the shown native
// window (docs/design/profiles-in-place.md). Its sidebar takes the tree first,
// and the shown window keeps its last frame until the sidebar and the page
// have painted in it. Owns itself.
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
    StrataSidebarView* sidebar = view ? FindSidebar(view) : nullptr;
    if (!sidebar || !sidebar->GetWebContents()) {
      Exchange();
      return;
    }
    // A hidden window's pages render as background ones do, slowly and
    // without frames; these render as if shown, so the window can change
    // sooner, and paint once it has.
    tabs::TabInterface* tab = window_->GetActiveTabInterface();
    for (content::WebContents* contents :
         {sidebar->GetWebContents(), tab ? tab->GetContents() : nullptr}) {
      if (contents) {
        captures_.push_back(contents->IncrementCapturerCount(
            gfx::Size(), /*stay_hidden=*/true, /*stay_awake=*/false,
            /*is_activity=*/false));
      }
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
    // Unless |first_paint|, a frame is enough: the sidebar has its tree, and
    // its first paint, made hidden, may never be reported.
    PaintWaiter(content::WebContents* contents,
                bool first_paint,
                base::OnceClosure painted)
        : painted_(std::move(painted)) {
      if (first_paint && !contents->CompletedFirstVisuallyNonEmptyPaint()) {
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
    for (auto [contents, first_paint] :
         {std::pair(sidebar ? sidebar->GetWebContents() : nullptr, false),
          std::pair(tab ? tab->GetContents() : nullptr, true)}) {
      if (contents) {
        waiters_.push_back(std::make_unique<PaintWaiter>(
            contents, first_paint,
            base::BindOnce(&ExchangeTransition::MaybeUnlock,
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
    // What the window put off so as not to slow the switch down.
    BrowserView* view =
        window_ ? BrowserView::GetBrowserViewForBrowser(window_.get())
                : nullptr;
    if (StrataSidebarView* sidebar = view ? FindSidebar(view) : nullptr) {
      sidebar->PreloadCommandBar();
    }
    // The new window's profile used its preloaded sidebar, and the profile
    // left may now need one.
    SidebarPreloader::Get()->RefillLater();
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
  std::vector<base::ScopedClosureRunner> captures_;
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
    (new ExchangeTransition(old, window, std::move(leave)))->Start();
  }));
}

}  // namespace strata
