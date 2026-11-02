// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_FIND_BAR_FIND_BAR_H_
#define COMPASS_BROWSER_UI_VIEWS_FIND_BAR_FIND_BAR_H_

#include <memory>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "compass/browser/ui/webui/find_bar/find_bar.mojom.h"
#include "components/find_in_page/find_result_observer.h"
#include "components/find_in_page/find_tab_helper.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/view_observer.h"

class BrowserWindowInterface;

namespace input {
struct NativeWebKeyboardEvent;
}  // namespace input

namespace content {
class WebContents;
}  // namespace content

namespace compass {

// A window's find bar (Cmd+F): chrome://compass-find over the top trailing
// corner of the active page, in place of upstream's find bar. It drives the
// active tab's find_in_page::FindTabHelper, which keeps each tab's text;
// each tab remembers whether its bar is open, so the bar follows tab
// switches. The page loads on the first Show() and stays loaded. See docs/design/claude.md,
// "Find bar".
//
// It's a WebView in the browser window, not a child widget as upstream's
// bar: the window stays the key window and its first responder takes the
// typing, as for the sidebar. Its page is transparent around the bar it
// draws, so the bar's shadow shows over the page.
class FindBar : public views::WebView,
                public find_bar::mojom::PageHandler,
                public find_in_page::FindResultObserver,
                public views::ViewObserver {
  METADATA_HEADER(FindBar, views::WebView)

 public:
  DECLARE_USER_DATA(FindBar);

  // The bar's size, its shadow included.
  static constexpr int kWidth = 380;
  static constexpr int kHeight = 64;

  // Adds one to |browser_view|, over its other children.
  static FindBar* Install(views::View* browser_view,
                          BrowserWindowInterface* browser);
  static FindBar* From(BrowserWindowInterface* browser);

  explicit FindBar(BrowserWindowInterface* browser);
  FindBar(const FindBar&) = delete;
  FindBar& operator=(const FindBar&) = delete;
  ~FindBar() override;

  // Opens the bar on the active tab, starting from the page's selection, or
  // the tab's last text, and focuses it. Open already, focuses it and
  // selects its text.
  void Show();
  bool IsShowing() const;
  // Cmd+G, Cmd+Shift+G: the next or previous match of the tab's text,
  // opening the bar if it's closed.
  void FindNextFromCommand(bool reverse);

  // The page's contents; null until the first Show().
  content::WebContents* page_contents() { return contents_.get(); }
  // The page connected (FindBarUI).
  void BindPage(mojo::PendingRemote<find_bar::mojom::Page> page,
                mojo::PendingReceiver<find_bar::mojom::PageHandler> receiver);
  bool IsPageConnectedForTesting() const { return page_.is_bound(); }
  const std::u16string& text_for_testing() const { return text_; }

  // find_bar::mojom::PageHandler:
  void Find(const std::string& text) override;
  void FindNext(bool reverse) override;
  void ForwardScrollKey(find_bar::mojom::ScrollKey key) override;
  // Closes the bar (also Esc and the close button), keeping the current match
  // selected in the page.
  void Close() override;

  // find_in_page::FindResultObserver:
  void OnFindResultAvailable(content::WebContents* web_contents) override;
  void OnFindEmptyText(content::WebContents* web_contents) override;
  void OnFindTabHelperDestroyed(find_in_page::FindTabHelper* helper) override;

  // content::WebContentsDelegate (the bar's page):
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;

  // views::ViewObserver (the browser view):
  void OnChildViewAdded(views::View* observed_view,
                        views::View* child) override;
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  // Watches the active tab for another page.
  class TabObserver : public content::WebContentsObserver {
   public:
    explicit TabObserver(FindBar* bar) : bar_(bar) {}
    void Watch(content::WebContents* contents) { Observe(contents); }
    // content::WebContentsObserver:
    void PrimaryPageChanged(content::Page& page) override;

   private:
    const raw_ptr<FindBar> bar_;
  };

  void OnPrimaryPageChanged();
  // The page's answer to Show()'s question for its selection.
  void OnPageSelection(base::WeakPtr<content::WebContents> contents,
                       base::Value result);
  void OnActiveTabChanged(BrowserWindowInterface* browser);
  // The active tab's helper; null without a tab.
  find_in_page::FindTabHelper* GetFindTabHelper();
  void CreatePageIfNeeded();
  // Sends the field's state to the page, once it's connected.
  void SendText(const std::u16string& text);
  void SendResult();
  void FocusPage();
  void UpdateBounds();
  void Hide(bool restore_focus);

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<content::WebContents> contents_;
  mojo::Remote<find_bar::mojom::Page> page_;
  mojo::Receiver<find_bar::mojom::PageHandler> receiver_{this};
  // What the field shows; the page tells every change.
  std::u16string text_;
  // The page asked to focus before it connected.
  bool focus_pending_ = false;
  // Something was typed since the bar opened.
  bool typed_since_show_ = false;

  base::ScopedObservation<find_in_page::FindTabHelper,
                          find_in_page::FindResultObserver>
      helper_observation_{this};
  base::ScopedObservation<views::View, views::ViewObserver> host_observation_{
      this};
  TabObserver tab_observer_{this};
  base::CallbackListSubscription active_tab_subscription_;
  ui::ScopedUnownedUserData<FindBar> scoped_data_holder_;
  base::WeakPtrFactory<FindBar> weak_factory_{this};
};

// chrome::FindInPage(), which every way to find goes through (the menu, the
// shortcuts, browser actions, IDC_FIND and IDC_FIND_NEXT/PREVIOUS), asks
// Compass first (see patches/). Returns true if the window's FindBar took it:
// |find_next| is Cmd+G (backwards unless |forward_direction|), otherwise
// Cmd+F.
bool HandleFindInPage(BrowserWindowInterface* browser,
                      bool find_next,
                      bool forward_direction);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_FIND_BAR_FIND_BAR_H_
