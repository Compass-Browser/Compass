// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/rooms/room_link_interceptor.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "compass/components/sync/rooms/room_invite_link.h"
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

namespace compass::rooms {

namespace {

bool IsBase64Url(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_';
  });
}

class RoomLinkInterceptor
    : public content::WebContentsObserver,
      public content::WebContentsUserData<RoomLinkInterceptor> {
 public:
  RoomLinkInterceptor(const RoomLinkInterceptor&) = delete;
  RoomLinkInterceptor& operator=(const RoomLinkInterceptor&) = delete;
  ~RoomLinkInterceptor() override = default;

  // content::WebContentsObserver:
  void DidStartNavigation(content::NavigationHandle* navigation) override {
    Check(navigation);
  }
  void DidRedirectNavigation(content::NavigationHandle* navigation) override {
    Check(navigation);
  }

 private:
  friend class content::WebContentsUserData<RoomLinkInterceptor>;

  explicit RoomLinkInterceptor(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        content::WebContentsUserData<RoomLinkInterceptor>(*web_contents) {}

  void Check(content::NavigationHandle* navigation) {
    if (!navigation->IsInPrimaryMainFrame() || navigation->IsSameDocument()) {
      return;
    }
    std::optional<std::string> fragment =
        JoinFragmentFromUrl(navigation->GetURL());
    if (!fragment || navigation->GetNavigationId() == handled_) {
      return;
    }
    handled_ = navigation->GetNavigationId();
    const content::NavigationEntry* committed =
        web_contents()->GetController().GetLastCommittedEntry();
    const bool fresh = !committed || committed->IsInitialEntry();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&RoomLinkInterceptor::OpenJoinPage,
                       weak_factory_.GetWeakPtr(), JoinPageUrlFor(*fragment),
                       fresh));
  }

  void OpenJoinPage(const GURL& page, bool fresh) {
    // The website never loads: the link's key stays in this browser.
    web_contents()->Stop();
    web_contents()->OpenURL(
        content::OpenURLParams(page, content::Referrer(),
                               fresh ? WindowOpenDisposition::kCurrentTab
                                     : WindowOpenDisposition::kNewForegroundTab,
                               ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                               /*is_renderer_initiated=*/false),
        /*navigation_handle_callback=*/{});
  }

  int64_t handled_ = 0;
  base::WeakPtrFactory<RoomLinkInterceptor> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(RoomLinkInterceptor);

}  // namespace

std::optional<std::string> JoinFragmentFromUrl(const GURL& url) {
  if (!url.is_valid() || !url.has_ref()) {
    return std::nullopt;
  }
  const bool web = url.SchemeIs(url::kHttpsScheme) &&
                   (url.host() == "compassbrowser.org" ||
                    url.host() == "www.compassbrowser.org") &&
                   (url.path() == "/join" || url.path() == "/join/" ||
                    url.path() == "/join.html");
  // compass://join#… loads as chrome://join#… (compass_url_handler.h).
  const bool app = (url.SchemeIs("chrome") || url.SchemeIs("compass")) &&
                   url.host() == "join";
  if (!web && !app) {
    return std::nullopt;
  }
  std::string fragment(url.ref());
  if (!IsBase64Url(fragment) ||
      fragment.size() > sync::rooms::kMaxInviteLinkSize) {
    return std::nullopt;
  }
  return fragment;
}

GURL JoinPageUrlFor(const std::string& fragment) {
  return GURL(base::StrCat({"chrome://compass-share/#join=", fragment}));
}

void AttachRoomLinkInterceptor(content::WebContents* web_contents) {
  RoomLinkInterceptor::CreateForWebContents(web_contents);
}

}  // namespace compass::rooms
