// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ROOMS_ROOM_LINK_INTERCEPTOR_H_
#define COMPASS_BROWSER_ROOMS_ROOM_LINK_INTERCEPTOR_H_

#include <optional>
#include <string>

class GURL;

namespace content {
class WebContents;
}  // namespace content

namespace compass::rooms {

// The fragment of an invite link (https://compassbrowser.org/join#…, or
// compass://join#…, which loads as chrome://join#…), or nullopt for any other
// URL. Only base64url fragments: anything else isn't a link Compass made.
std::optional<std::string> JoinFragmentFromUrl(const GURL& url);

// chrome://compass-share/#join=<fragment>: the join page.
GURL JoinPageUrlFor(const std::string& fragment);

// A tab opening an invite link gets Compass's join page instead of the
// website (which only explains the link): in the same tab if it had nothing
// loaded yet (a link from another app, a new tab), else in a new tab next to
// it. The fragment (the room's key and the invite's secret) never reaches a
// server. As BoostLinkInterceptor; docs/design/sync.md, "Rooms: clients".
// Upstream's TabHelpers::AttachTabHelpers() calls this for every tab (see
// patches/chrome-browser-ui-tab_helpers.cc.patch).
void AttachRoomLinkInterceptor(content::WebContents* web_contents);

}  // namespace compass::rooms

#endif  // COMPASS_BROWSER_ROOMS_ROOM_LINK_INTERCEPTOR_H_
