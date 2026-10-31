// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_PEEK_PEEK_H_
#define COMPASS_BROWSER_UI_VIEWS_PEEK_PEEK_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "content/public/browser/invalidate_type.h"
#include "content/public/browser/web_contents_delegate.h"
#include "ui/base/window_open_disposition.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"

class BrowserWindowInterface;

namespace content {
struct OpenURLParams;
class WebContents;
}  // namespace content

namespace tabs {
class TabInterface;
}  // namespace tabs

namespace views {
class WebView;
}  // namespace views

namespace compass {

class ActiveTabWatcher;
class PeekView;

// A window's Peek (docs/design/peek.md): Option+click on a link shows its page
// in a card over the content area instead of opening a tab. Esc, Cmd+W, the
// close button, a click beside the card, switching tabs or clicking the rest
// of the window closes it; Cmd+O or the open button makes the page a Today
// entry (the same WebContents, so it doesn't reload). The page isn't in the
// strip or the sidebar until then. Owned by the window's sidebar view.
class Peek : public content::WebContentsDelegate,
             public views::WidgetObserver,
             public views::ViewObserver {
 public:
  DECLARE_USER_DATA(Peek);

  explicit Peek(BrowserWindowInterface* browser);
  Peek(const Peek&) = delete;
  Peek& operator=(const Peek&) = delete;
  ~Peek() override;

  static Peek* From(BrowserWindowInterface* browser);

  // Shows |params|'s URL, opened from |source| (a tab of the window). If a
  // page is showing already, it goes there instead.
  void Open(content::WebContents* source, const content::OpenURLParams& params);
  // Shows |contents|, a page |source| opened in a new tab (a link from a
  // pinned tab), in place of any page showing.
  void Adopt(content::WebContents* source,
             std::unique_ptr<content::WebContents> contents);
  // Fades the card out, then lets go of the page.
  void Close();
  // Moves the page into the window as the active tab: a new Today entry.
  void OpenAsEntry();

  // False once closing starts.
  bool IsShowing() const { return widget_ && !closing_; }
  // The page; null when closed.
  content::WebContents* web_contents() const { return contents_.get(); }
  views::Widget* widget_for_testing() const { return widget_.get(); }
  // The title and host above the card.
  std::u16string GetTitleForTesting() const;
  std::u16string GetHostForTesting() const;

  // content::WebContentsDelegate:
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  void CloseContents(content::WebContents* source) override;
  void NavigationStateChanged(content::WebContents* source,
                              content::InvalidateTypes changed_flags) override;
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override;
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params) override;
  void EnumerateDirectory(content::WebContents* web_contents,
                          scoped_refptr<content::FileSelectListener> listener,
                          const base::FilePath& path) override;
  void RequestMediaAccessPermission(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* render_frame_host,
                                  const url::Origin& security_origin,
                                  blink::mojom::MediaStreamType type) override;

  // views::WidgetObserver:
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetDestroying(views::Widget* widget) override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  void Show();
  // Runs |action| after the current event: the page or the card may be in
  // the middle of handling it.
  void PostAction(void (Peek::*action)());
  // Puts the card where the content area is.
  void UpdateBounds();
  // Closes at once, without the animation.
  void FinishClose();
  // Stops following the window, its content area and its active tab.
  void StopFollowing();
  void FocusActiveTab();
  // Lets go of the card and the page, which go after the current event.
  void Teardown();
  void OnActiveTabChanged(const tabs::TabInterface* tab);

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<content::WebContents> contents_;
  std::unique_ptr<views::Widget> widget_;
  // The widget's delegate; outlives it.
  std::unique_ptr<views::WidgetDelegate> widget_delegate_;
  raw_ptr<PeekView> peek_view_ = nullptr;
  raw_ptr<views::WebView> web_view_ = nullptr;
  // Set while the card fades out.
  bool closing_ = false;
  // The tab the page was opened from; Peek closes when another one shows.
  raw_ptr<const tabs::TabInterface> source_tab_ = nullptr;
  std::unique_ptr<ActiveTabWatcher> active_tab_watcher_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      browser_widget_observation_{this};
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  base::ScopedObservation<views::View, views::ViewObserver>
      content_area_observation_{this};

  ui::ScopedUnownedUserData<Peek> scoped_data_holder_;
  base::WeakPtrFactory<Peek> weak_factory_{this};
};

// Option+click on a link in |source|, a tab of |browser| (OpenURLFromTab with
// SAVE_TO_DISK, see patches/): peeks at it in a window with Peek, otherwise
// downloads it, as upstream does for every Option+click. Called from
// BrowserWebContentsDelegate::OpenURLFromTab().
void PeekOrDownload(BrowserWindowInterface* browser,
                    content::WebContents* source,
                    const content::OpenURLParams& params);

// A link from a pinned tab or favorite of |browser| that opens a new
// foreground tab (target=_blank, window.open() on a click) shows in Peek
// instead, as in Arc. Returns the page if Peek took |new_contents|, else null
// and leaves it. Called from BrowserWebContentsDelegate::AddNewContents().
content::WebContents* MaybePeekNewContents(
    BrowserWindowInterface* browser,
    content::WebContents* source,
    std::unique_ptr<content::WebContents>& new_contents,
    WindowOpenDisposition disposition,
    bool user_gesture);

// Browser commands while Peek shows: IDC_CLOSE_TAB closes it instead of the
// tab, IDC_OPEN_FILE opens it as an entry. Returns true if it handled |id|.
// Called from compass::HandleBrowserCommand().
bool HandlePeekCommand(BrowserWindowInterface* browser, int id);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_PEEK_PEEK_H_
