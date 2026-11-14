// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_READER_READER_TAB_STATE_H_
#define COMPASS_BROWSER_UI_VIEWS_READER_READER_TAB_STATE_H_

#include <memory>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/views/view_observer.h"
#include "url/gurl.h"

class BrowserView;
class ContentsContainerView;

namespace views {
class WebView;
}  // namespace views

namespace compass {

// A tab's Reader mode (docs/design/reader-mode.md): whether its page reads
// as an article (common/reader/reader.js check(), in an isolated world), and,
// while in Reader, the reader view: a WebContents showing
// chrome-untrusted://compass-reader/ with the article, in a views::WebView
// over the pane that shows the tab. The tab's own WebContents is left alone,
// so its URL, history and state are as they were when Reader ends.
//
// User data of the tab's WebContents, made by ReaderMode for tabs that come
// to the front in a window with a sidebar. ReaderMode puts the view over the
// right pane (AttachTo()).
class ReaderTabState : public content::WebContentsUserData<ReaderTabState>,
                       public content::WebContentsObserver,
                       public content::WebContentsDelegate,
                       public views::ViewObserver {
 public:
  ReaderTabState(const ReaderTabState&) = delete;
  ReaderTabState& operator=(const ReaderTabState&) = delete;
  ~ReaderTabState() override;

  // Runs |callback| with the tab's WebContents whenever a tab's Reader state
  // changes (readerable, showing).
  static base::CallbackListSubscription RegisterChanged(
      base::RepeatingCallback<void(content::WebContents*)> callback);

  // Whether |url|'s pages may be shown in Reader: web pages and files.
  static bool CanRead(const GURL& url);

  // The page reads as an article.
  bool readerable() const { return readerable_; }
  // In Reader: the reader view is (or is being) shown.
  bool showing() const { return showing_; }
  // Reader can be entered: a web page, not in Reader or on the way there.
  bool CanEnter() const;

  // Reads the page and shows the reader view. A page with no article shows
  // the view's message for that.
  void Enter();
  // Back to the live page.
  void Leave();

  // Puts the reader view over |container|, the pane that shows the tab, or
  // takes it away (null).
  void AttachTo(ContentsContainerView* container);

  // Checks again whether the page is an article.
  void Check();

  content::WebContents* reader_contents_for_testing() const {
    return reader_contents_.get();
  }
  views::WebView* view_for_testing() const { return view_; }
  // A check of the current document has answered.
  bool checked_for_testing() const { return checked_; }

  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;
  void PrimaryPageChanged(content::Page& page) override;
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;

  // content::WebContentsDelegate, of the reader view's WebContents:
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
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override;
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  friend class content::WebContentsUserData<ReaderTabState>;
  explicit ReaderTabState(content::WebContents* contents);

  void OnChecked(int generation, base::Value result);
  void OnParsed(int parse_id, base::Value result);
  // Loads the reader view with |json|, reader.js parse()'s result.
  void Show(std::string json);
  // Lets go of the view and its WebContents.
  void Teardown();
  void SetReaderable(bool readerable);
  void NotifyChanged();
  // The window the tab is in, if any.
  BrowserView* GetBrowserView() const;
  bool IsActiveTab() const;
  void PostLeave();

  bool readerable_ = false;
  bool showing_ = false;
  bool parsing_ = false;
  bool checked_ = false;
  // Counts the tab's documents, so a check of an older one is dropped.
  int generation_ = 0;
  // Counts Enter() calls, so a parse a Leave() came after is dropped.
  int parse_id_ = 0;
  // The article's token in ReaderArticleStore while shown.
  std::string token_;
  std::unique_ptr<content::WebContents> reader_contents_;
  // Over |container_|, which owns it.
  raw_ptr<views::WebView> view_ = nullptr;
  raw_ptr<ContentsContainerView> container_ = nullptr;
  base::ScopedMultiSourceObservation<views::View, views::ViewObserver>
      view_observations_{this};
  base::CallbackListSubscription detached_subscription_;
  // Checks again a while after a same-document navigation, or after load
  // for pages that build their text late.
  base::OneShotTimer recheck_timer_;
  base::WeakPtrFactory<ReaderTabState> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_READER_READER_TAB_STATE_H_
