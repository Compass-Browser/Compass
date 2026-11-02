// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_PAGE_READER_H_
#define COMPASS_BROWSER_CLAUDE_PAGE_READER_H_

#include <string>

#include "base/functional/callback_forward.h"
#include "base/types/expected.h"
#include "base/values.h"

class GURL;

namespace content {
class WebContents;
}  // namespace content

namespace compass::claude {

struct PageText {
  std::string url;
  std::string title;
  std::string text;
  // The text was cut at the limit.
  bool truncated = false;
};

// Whether Claude may read and act on pages at |url|: web pages and files,
// not the browser's own pages (chrome://, compass://, DevTools, extensions,
// the Web Store), which hold settings and passwords. See
// docs/design/claude.md, "Safety".
bool CanUsePage(const GURL& url);

// Calls __compassAgent.<method>(opts) of common/claude/page_agent.js in an
// isolated world of |contents|' main frame, so the page's own scripts can't
// see or change it. |callback| gets the agent's result without its "ok", or
// its error.
void RunPageAgent(
    content::WebContents* contents,
    const std::string& method,
    base::DictValue opts,
    base::OnceCallback<void(base::expected<base::DictValue, std::string>)>
        callback);

// The main text of |contents|' page, through common/claude/page_agent.js in
// an isolated world, so the page's own scripts can't see or change it. At
// most |max_chars| characters. An error says why not, for the user.
void ReadPageText(content::WebContents* contents,
                  int max_chars,
                  base::OnceCallback<void(base::expected<PageText, std::string>)>
                      callback);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_PAGE_READER_H_
