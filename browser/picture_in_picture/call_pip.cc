// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/picture_in_picture/call_pip.h"

#include <string>
#include <string_view>

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/picture_in_picture/call_pip_assets.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
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

 private:
  friend class content::WebContentsUserData<CallPip>;

  explicit CallPip(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        content::WebContentsUserData<CallPip>(*web_contents) {}

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
  const std::string_view path = url.path();
  return IsHostOrSubdomain(host, kZoomHost) &&
         (path == "/wc" || path.starts_with("/wc/"));
}

}  // namespace compass
