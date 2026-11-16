// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/boost_link_interceptor.h"

#include <cstdint>
#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "compass/browser/boosts/boost_share.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass::boosts {

namespace {

class BoostLinkInterceptor
    : public content::WebContentsObserver,
      public content::WebContentsUserData<BoostLinkInterceptor> {
 public:
  BoostLinkInterceptor(const BoostLinkInterceptor&) = delete;
  BoostLinkInterceptor& operator=(const BoostLinkInterceptor&) = delete;
  ~BoostLinkInterceptor() override = default;

  // content::WebContentsObserver:
  void DidStartNavigation(content::NavigationHandle* navigation) override {
    Check(navigation);
  }
  void DidRedirectNavigation(content::NavigationHandle* navigation) override {
    Check(navigation);
  }

 private:
  friend class content::WebContentsUserData<BoostLinkInterceptor>;

  explicit BoostLinkInterceptor(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        content::WebContentsUserData<BoostLinkInterceptor>(*web_contents) {}

  void Check(content::NavigationHandle* navigation) {
    if (!navigation->IsInPrimaryMainFrame() || navigation->IsSameDocument()) {
      return;
    }
    std::optional<std::string> fragment =
        ShareFragmentFromUrl(navigation->GetURL());
    if (!fragment || navigation->GetNavigationId() == handled_) {
      return;
    }
    // Once per navigation, should it redirect to another Boost link.
    handled_ = navigation->GetNavigationId();
    const content::NavigationEntry* committed =
        web_contents()->GetController().GetLastCommittedEntry();
    const bool fresh = !committed || committed->IsInitialEntry();
    // Not from inside the observer call: stopping a navigation that's
    // starting is up to the task.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&BoostLinkInterceptor::OpenPreview,
                       weak_factory_.GetWeakPtr(), ImportPreviewUrl(*fragment),
                       fresh));
  }

  void OpenPreview(const GURL& preview, bool fresh) {
    // The website never loads: the payload stays in this browser.
    web_contents()->Stop();
    // Off the record, chrome::Navigate() opens settings in a normal window.
    web_contents()->OpenURL(
        content::OpenURLParams(
            preview, content::Referrer(),
            fresh ? WindowOpenDisposition::CURRENT_TAB
                  : WindowOpenDisposition::NEW_FOREGROUND_TAB,
            ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
            /*is_renderer_initiated=*/false),
        /*navigation_handle_callback=*/{});
  }

  int64_t handled_ = 0;
  base::WeakPtrFactory<BoostLinkInterceptor> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(BoostLinkInterceptor);

}  // namespace

void AttachBoostLinkInterceptor(content::WebContents* web_contents) {
  BoostLinkInterceptor::CreateForWebContents(web_contents);
}

}  // namespace compass::boosts
