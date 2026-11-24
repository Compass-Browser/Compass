// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SYNC_CLOUDFLARE_SIGN_IN_H_
#define COMPASS_BROWSER_SYNC_CLOUDFLARE_SIGN_IN_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "compass/components/sync/cloudflare_oauth.h"
#include "content/public/browser/web_contents_observer.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

class Profile;

namespace content {
class NavigationHandle;
}  // namespace content

namespace compass::sync {

// "Sign in with Cloudflare" (docs/design/sync.md, "A free server on
// Cloudflare"): opens Cloudflare's sign-in in a new tab of |profile|'s
// window, catches the navigation to the redirect URI there (it never
// loads: the tab closes), checks its state, and trades its code for an
// access token. Destroying |this| ends it without running the callback.
class CloudflareSignIn : public content::WebContentsObserver {
 public:
  // The access token, or nullopt and a sentence for the page.
  using Callback =
      base::OnceCallback<void(std::optional<std::string> token,
                              std::string error)>;

  CloudflareSignIn(Profile* profile,
                   scoped_refptr<network::SharedURLLoaderFactory> factory);
  CloudflareSignIn(const CloudflareSignIn&) = delete;
  CloudflareSignIn& operator=(const CloudflareSignIn&) = delete;
  ~CloudflareSignIn() override;

  void Start(Callback callback);

  // content::WebContentsObserver:
  void DidStartNavigation(content::NavigationHandle* navigation) override;
  void DidRedirectNavigation(content::NavigationHandle* navigation) override;
  void WebContentsDestroyed() override;

 private:
  void OnNavigation(const GURL& url);
  void CloseTab();
  void Finish(std::optional<std::string> token, std::string error);

  raw_ptr<Profile> profile_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  CloudflareAuthRequest request_;
  Callback callback_;
  // Set once the redirect came: the tab closing after it isn't a cancel.
  bool redirected_ = false;
  std::unique_ptr<CloudflareTokenExchange> exchange_;
  base::WeakPtrFactory<CloudflareSignIn> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_BROWSER_SYNC_CLOUDFLARE_SIGN_IN_H_
