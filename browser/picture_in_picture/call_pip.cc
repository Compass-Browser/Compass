// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/picture_in_picture/call_pip.h"

#include <string>
#include <string_view>

#include "base/functional/callback_helpers.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/picture_in_picture/call_pip_assets.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace compass {

namespace {

// As in call_pip.js's SITES: a host or its subdomains, and for Zoom the web
// client's path.
constexpr std::string_view kTeamsHosts[] = {
    "teams.microsoft.com",
    "teams.live.com",
    "teams.cloud.microsoft",
};
constexpr std::string_view kZoomHost = "zoom.us";

bool IsHostOrSubdomain(std::string_view host, std::string_view domain) {
  return host == domain ||
         (host.size() > domain.size() && host.ends_with(domain) &&
          host[host.size() - domain.size() - 1] == '.');
}

// Zoom's web client: /wc/ on zoom.us or a subdomain.
bool IsZoomWebClientURL(const GURL& url) {
  const std::string_view path = url.path();
  return url.SchemeIs(url::kHttpsScheme) &&
         IsHostOrSubdomain(url.host(), kZoomHost) &&
         (path == "/wc" || path.starts_with("/wc/"));
}

class CallPip : public content::WebContentsObserver,
                public content::WebContentsUserData<CallPip> {
 public:
  ~CallPip() override = default;

  // content::WebContentsObserver:
  // Any frame of the shown page whose own URL is a call's: Zoom's web
  // client runs the call in a same-origin <iframe> of its /wc/ page.
  void DOMContentLoaded(content::RenderFrameHost* frame) override {
    if (!frame->GetMainFrame()->IsInPrimaryMainFrame() ||
        !IsCallPipURL(frame->GetLastCommittedURL())) {
      return;
    }
    frame->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(call_pip::assets::kCallPip), base::NullCallback(),
        ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }

  // Zoom's web client drops the participants' video while its page is
  // hidden, which empties (or freezes) the picture-in-picture window as soon
  // as its tab is left. While the tab shows it, the page is told it's
  // visible whatever the tab's state, as for a tab being captured: Compass's
  // own tab and window checks see the real state.
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (!navigation->IsInPrimaryMainFrame() || !navigation->HasCommitted()) {
      return;
    }
    const bool zoom = IsZoomWebClientURL(navigation->GetURL());
    if (zoom && !keep_visible_) {
      keep_visible_ = web_contents()->IncrementCapturerCount(
          gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/false,
          /*is_activity=*/false);
    } else if (!zoom && keep_visible_) {
      keep_visible_.RunAndReset();
    }
  }

  bool keeps_visible_for_testing() const { return !!keep_visible_; }

 private:
  friend class content::WebContentsUserData<CallPip>;

  explicit CallPip(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        content::WebContentsUserData<CallPip>(*web_contents) {}

  base::ScopedClosureRunner keep_visible_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(CallPip);

}  // namespace

void AttachCallPip(content::WebContents* web_contents) {
  CallPip::CreateForWebContents(web_contents);
}

bool IsCallPipURL(const GURL& url) {
  if (!url.SchemeIs(url::kHttpsScheme)) {
    return false;
  }
  const std::string_view host = url.host();
  for (std::string_view teams : kTeamsHosts) {
    if (IsHostOrSubdomain(host, teams)) {
      return true;
    }
  }
  return IsZoomWebClientURL(url);
}

bool CallPipKeepsPageVisibleForTesting(content::WebContents* web_contents) {
  CallPip* call_pip = CallPip::FromWebContents(web_contents);
  return call_pip && call_pip->keeps_visible_for_testing();
}

}  // namespace compass
