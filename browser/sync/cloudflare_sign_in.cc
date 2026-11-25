// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sync/cloudflare_sign_in.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#if BUILDFLAG(IS_ANDROID)
// Chrome's tab models; their headers come with //chrome/browser, which
// this links into (as //compass/browser/android's ClaudeTabsAndroid).
#include "chrome/browser/android/tab_android.h"  // nogncheck
#include "chrome/browser/ui/android/tab_model/tab_model.h"  // nogncheck
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"  // nogncheck
#else
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"  // nogncheck
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"  // nogncheck
#include "chrome/browser/ui/navigator/browser_navigator.h"  // nogncheck
#include "chrome/browser/ui/navigator/browser_navigator_params.h"  // nogncheck
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#endif

namespace compass::sync {

CloudflareSignIn::CloudflareSignIn(
    Profile* profile,
    scoped_refptr<network::SharedURLLoaderFactory> factory)
    : profile_(profile), factory_(std::move(factory)) {}

CloudflareSignIn::~CloudflareSignIn() {
  // Ended early (the user left the setup, or the profile shut down): a
  // sign-in tab still open goes too.
  if (!redirected_) {
    CloseTab();
  }
}

void CloudflareSignIn::Start(Callback callback) {
  callback_ = std::move(callback);
  request_ = MakeCloudflareAuthRequest();
#if BUILDFLAG(IS_ANDROID)
  // A Compass tab, not a Custom Tab: only a tab of Compass's own lets it
  // see the redirect to an https URI (docs/design/android.md, "Sync and
  // push"). In this profile's regular tabs, the window on screen first.
  TabModel* model = nullptr;
  for (TabModel* candidate : TabModelList::models()) {
    if (candidate->IsOffTheRecord() ||
        candidate->GetProfile() != profile_.get()) {
      continue;
    }
    if (!model || candidate->IsActiveModel()) {
      model = candidate;
    }
  }
  if (!model) {
    Finish(std::nullopt, "Open a Compass window and try again.");
    return;
  }
  tabs::TabInterface* tab = model->OpenTab(request_.url, model->GetTabCount(),
                                           /*foreground=*/true);
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    Finish(std::nullopt, "Couldn't open Cloudflare's sign-in page.");
    return;
  }
  Observe(contents);
#else
  // This profile's window, else any normal one.
  BrowserWindowInterface* window = nullptr;
  BrowserWindowInterface* any = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
          return true;
        }
        if (!any) {
          any = browser;
        }
        if (browser->GetProfile() == profile_.get()) {
          window = browser;
          return false;
        }
        return true;
      });
  if (!window) {
    window = any;
  }
  if (!window) {
    Finish(std::nullopt, "Open a Compass window and try again.");
    return;
  }
  NavigateParams params(window, request_.url, ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
  if (!params.navigated_or_inserted_contents) {
    Finish(std::nullopt, "Couldn't open Cloudflare's sign-in page.");
    return;
  }
  Observe(params.navigated_or_inserted_contents);
#endif
}

void CloudflareSignIn::DidStartNavigation(
    content::NavigationHandle* navigation) {
  if (navigation->IsInPrimaryMainFrame()) {
    OnNavigation(navigation->GetURL());
  }
}

void CloudflareSignIn::DidRedirectNavigation(
    content::NavigationHandle* navigation) {
  // Cloudflare answers the consent with a redirect to the redirect URI.
  if (navigation->IsInPrimaryMainFrame()) {
    OnNavigation(navigation->GetURL());
  }
}

void CloudflareSignIn::WebContentsDestroyed() {
  Observe(nullptr);
  if (!redirected_) {
    Finish(std::nullopt, "Cloudflare sign-in was closed before it finished.");
  }
}

void CloudflareSignIn::OnNavigation(const GURL& url) {
  if (redirected_ || !IsCloudflareRedirect(url)) {
    return;
  }
  redirected_ = true;
  // The page itself isn't wanted (the code is useless to it without the
  // verifier, which never leaves Compass): the tab goes, after this
  // navigation callback returns.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&CloudflareSignIn::CloseTab,
                                weak_factory_.GetWeakPtr()));
  const CloudflareAuthReply reply =
      ParseCloudflareRedirect(url, request_.state);
  if (!reply.code) {
    Finish(std::nullopt, reply.error == "access_denied"
                             ? "You didn't allow Compass on Cloudflare."
                             : "Cloudflare sign-in didn't finish. Try "
                               "again.");
    return;
  }
  exchange_ = std::make_unique<CloudflareTokenExchange>(factory_);
  exchange_->Exchange(
      *reply.code, request_.verifier,
      base::BindOnce(
          [](base::WeakPtr<CloudflareSignIn> self,
             std::optional<std::string> token) {
            if (!self) {
              return;
            }
            if (!token) {
              self->Finish(std::nullopt,
                           "Cloudflare didn't give Compass access. Try "
                           "again.");
              return;
            }
            self->Finish(std::move(token), std::string());
          },
          weak_factory_.GetWeakPtr()));
}

void CloudflareSignIn::CloseTab() {
  content::WebContents* contents = web_contents();
  Observe(nullptr);
  if (!contents) {
    return;
  }
#if BUILDFLAG(IS_ANDROID)
  if (TabAndroid* tab = TabAndroid::FromWebContents(contents)) {
    if (TabModel* model = TabModelList::GetTabModelForTabAndroid(tab)) {
      model->CloseTab(tab->GetHandle());
    }
  }
#else
  if (tabs::TabInterface* tab =
          tabs::TabInterface::MaybeGetFromContents(contents)) {
    tab->Close();
  }
#endif
}

void CloudflareSignIn::Finish(std::optional<std::string> token,
                              std::string error) {
  if (callback_) {
    std::move(callback_).Run(std::move(token), std::move(error));
  }
}

}  // namespace compass::sync
