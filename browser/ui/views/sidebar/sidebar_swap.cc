// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/sidebar/sidebar_swap.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/desktop_browser_window_capabilities.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "compass/browser/ui/views/frame/browser_view_exchange.h"
#include "compass/browser/ui/views/sidebar/sidebar_header.h"
#include "compass/browser/ui/views/sidebar/sidebar_tint.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
#include "compass/browser/ui/webui/sidebar/compass_sidebar_ui.h"
#include "compass/browser/window_sync/window_features.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_lock.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

// The window never holds its last frame longer.
constexpr base::TimeDelta kMaxSwapWait = base::Milliseconds(750);
// A page still loading paints in place after this: the window doesn't wait
// for the network, as Arc's doesn't.
constexpr base::TimeDelta kMaxPagePaintWait = base::Milliseconds(200);
// A sidebar built hidden takes its tree in ~0.5 s in an optimized build, and
// the window doesn't change until it has.
constexpr base::TimeDelta kMaxTreeWait = base::Seconds(2);
base::TimeDelta g_max_tree_wait = kMaxTreeWait;

bool g_installed = false;

CompassSidebarView* FindSidebar(views::View* view) {
  if (auto* sidebar = views::AsViewClass<CompassSidebarView>(view)) {
    return sidebar;
  }
  for (views::View* child : view->children()) {
    if (CompassSidebarView* sidebar = FindSidebar(child)) {
      return sidebar;
    }
  }
  return nullptr;
}

// |view|'s tints, in tree order.
void CollectTints(views::View* view, std::vector<SidebarTint*>& tints) {
  if (auto* tint = views::AsViewClass<SidebarTint>(view)) {
    tints.push_back(tint);
  }
  for (views::View* child : view->children()) {
    CollectTints(child, tints);
  }
}

std::vector<SidebarTint*> FindTints(views::View* view) {
  std::vector<SidebarTint*> tints;
  CollectTints(view, tints);
  return tints;
}

// The new profile's window, built hidden, gives its view to the shown native
// window (docs/design/profiles-in-place.md). Its sidebar takes the old one's
// page, which slides to the new space as the switch starts and takes the new
// tree once it has the window, as within a profile; the shown window keeps
// its last frame until the page has painted in it. Owns itself.
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
    BrowserView* old_view =
        old_ ? BrowserView::GetBrowserViewForBrowser(old_.get()) : nullptr;
    CompassSidebarView* sidebar = view ? FindSidebar(view) : nullptr;
    CompassSidebarView* old_sidebar = old_view ? FindSidebar(old_view) : nullptr;
    if (!sidebar) {
      Exchange();
      return;
    }
    takes_page_ = old_sidebar && old_sidebar->GetWebContents() &&
                  !sidebar->GetWebContents();
    if (!takes_page_) {
      // No page to take: the window loads its own, and changes once it has
      // its tree.
      sidebar->LoadPage();
    }
    // A hidden window's pages render as background ones do, slowly and
    // without frames; these render as if shown, so the window can change
    // sooner, and paint once it has. The page that moves goes hidden as it
    // leaves the old window, and would have no frame for the new one.
    tabs::TabInterface* tab = window_->GetActiveTabInterface();
    for (content::WebContents* contents :
         {takes_page_ ? old_sidebar->GetWebContents()
                      : sidebar->GetWebContents(),
          tab ? tab->GetContents() : nullptr}) {
      if (contents) {
        captures_.push_back(contents->IncrementCapturerCount(
            gfx::Size(), /*stay_hidden=*/true, /*stay_awake=*/false,
            /*is_activity=*/false));
      }
    }
    if (takes_page_) {
      // The page goes on sliding, in its own renderer, and takes the new
      // tree mid-slide as within a profile: the rest of the window needn't
      // wait for it.
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
    // A frame is enough for what has its content: the sidebar, which has its
    // tree, and a page that has loaded. A first paint made hidden may never
    // be reported. A page (|first_paint|) still loading waits for its first
    // paint or, failing that, a frame once it has loaded; a page waits at
    // most kMaxPagePaintWait.
    PaintWaiter(content::WebContents* contents,
                bool first_paint,
                base::OnceClosure painted)
        : painted_(std::move(painted)) {
      if (first_paint) {
        deadline_.Start(FROM_HERE, kMaxPagePaintWait,
                        base::BindOnce(&PaintWaiter::Painted,
                                       base::Unretained(this)));
      }
      if (first_paint && !contents->CompletedFirstVisuallyNonEmptyPaint() &&
          contents->IsLoading()) {
        Observe(contents);
        return;
      }
      WaitForFrame(contents);
    }

    void DidFirstVisuallyNonEmptyPaint() override { Painted(); }
    void DidStopLoading() override {
      content::WebContents* contents = web_contents();
      Observe(nullptr);
      WaitForFrame(contents);
    }
    void WebContentsDestroyed() override { Painted(); }

    bool waiting() const { return !painted_.is_null(); }

   private:
    void WaitForFrame(content::WebContents* contents) {
      contents->GetPrimaryMainFrame()->InsertVisualStateCallback(
          base::BindOnce(
              [](base::WeakPtr<PaintWaiter> waiter, bool) {
                if (waiter) {
                  waiter->Painted();
                }
              },
              weak_factory_.GetWeakPtr()));
    }

    void Painted() {
      deadline_.Stop();
      Observe(nullptr);
      if (painted_) {
        std::move(painted_).Run();
      }
    }

    base::OnceClosure painted_;
    base::OneShotTimer deadline_;
    base::WeakPtrFactory<PaintWaiter> weak_factory_{this};
  };

  void WaitForTree() {
    content::WebUI* web_ui = web_contents()->GetWebUI();
    CompassSidebarUI* ui =
        web_ui ? web_ui->GetController()->GetAs<CompassSidebarUI>() : nullptr;
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
    // Not while either window closes (when quitting, for one).
    if (!old_view || !view || browser_shutdown::IsTryingToQuit() ||
        old_->capabilities()->IsAttemptingToCloseBrowser() ||
        window_->capabilities()->IsAttemptingToCloseBrowser()) {
      Finish();
      return;
    }
    BrowserViewExchange::Exchange(*old_view, *view);
    CompassSidebarView* old_sidebar = FindSidebar(old_view);
    if (CompassSidebarView* sidebar = FindSidebar(view);
        takes_page_ && sidebar && old_sidebar) {
      // Shown already, as the slide ended: it stays put, and shows the new
      // window's tree once it has it.
      sidebar->TakePageFrom(*old_sidebar);
    }
    // The tints (the region's and the frame's) go on from the old window's.
    std::vector<SidebarTint*> tints = FindTints(view);
    std::vector<SidebarTint*> old_tints = FindTints(old_view);
    for (size_t i = 0; i < tints.size() && i < old_tints.size(); ++i) {
      tints[i]->ContinueFrom(*old_tints[i]);
    }
    // And the header, whose extensions differ.
    SidebarHeader* header = SidebarHeader::From(window_.get());
    SidebarHeader* old_header = old_ ? SidebarHeader::From(old_.get()) : nullptr;
    if (header && old_header) {
      header->ContinueFrom(*old_header);
    }

    if (ui::Compositor* compositor = view->GetWidget()->GetCompositor()) {
      lock_ = compositor->GetCompositorLock(this, kMaxSwapWait);
    }
    CompassSidebarView* sidebar = FindSidebar(view);
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
    if (CompassSidebarView* sidebar = view ? FindSidebar(view) : nullptr) {
      sidebar->PreloadCommandBar();
    }
    delete this;
  }

  base::WeakPtr<BrowserWindowInterface> old_;
  base::WeakPtr<BrowserWindowInterface> window_;
  base::OnceClosure leave_;
  base::OneShotTimer timeout_;
  // The new window takes the old one's sidebar page.
  bool takes_page_ = false;
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

}  // namespace compass
