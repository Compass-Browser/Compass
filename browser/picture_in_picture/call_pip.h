// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_PICTURE_IN_PICTURE_CALL_PIP_H_
#define COMPASS_BROWSER_PICTURE_IN_PICTURE_CALL_PIP_H_

class GURL;

namespace content {
class WebContents;
}  // namespace content

namespace compass {

// Picture-in-picture for calls on Teams and Zoom, which don't offer their
// own: common/picture_in_picture/call_pip.js, run in an isolated world of
// their pages (docs/design/picture-in-picture.md, "Calls"). Attached to
// every tab by upstream's TabHelpers (see patches/).
void AttachCallPip(content::WebContents* web_contents);

// Whether |url| is a call site the script is for (it checks again).
bool IsCallPipURL(const GURL& url);

}  // namespace compass

#endif  // COMPASS_BROWSER_PICTURE_IN_PICTURE_CALL_PIP_H_
