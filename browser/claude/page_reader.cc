// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/page_reader.h"

#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "compass/browser/claude/claude_assets.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace compass::claude {

namespace {

using Callback =
    base::OnceCallback<void(base::expected<PageText, std::string>)>;

void OnResult(Callback callback, base::Value result) {
  const base::DictValue* dict = result.GetIfDict();
  if (!dict) {
    std::move(callback).Run(
        base::unexpected("The page couldn't be read (it may still be "
                         "loading)."));
    return;
  }
  if (!dict->FindBool("ok").value_or(false)) {
    const std::string* error = dict->FindString("error");
    std::move(callback).Run(base::unexpected(
        error ? *error : std::string("The page couldn't be read.")));
    return;
  }
  PageText page;
  if (const std::string* url = dict->FindString("url")) {
    page.url = *url;
  }
  if (const std::string* title = dict->FindString("title")) {
    page.title = *title;
  }
  if (const std::string* text = dict->FindString("text")) {
    page.text = *text;
  }
  page.truncated = dict->FindBool("truncated").value_or(false);
  std::move(callback).Run(std::move(page));
}

}  // namespace

bool CanUsePage(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  if (!url.SchemeIsHTTPOrHTTPS() && !url.SchemeIsFile()) {
    return false;
  }
  // The Web Store installs extensions, which could change the browser.
  const std::string_view host = url.host();
  return host != "chromewebstore.google.com" &&
         !(host == "chrome.google.com" && url.path().starts_with("/webstore"));
}

void ReadPageText(content::WebContents* contents,
                  int max_chars,
                  Callback callback) {
  const GURL& url = contents->GetLastCommittedURL();
  if (!CanUsePage(url)) {
    std::move(callback).Run(base::unexpected(
        "Claude can't read the browser's own pages, only web pages."));
    return;
  }
  content::RenderFrameHost* frame = contents->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    std::move(callback).Run(base::unexpected("The page isn't loaded yet."));
    return;
  }
  // The agent installs itself once per document; the last expression is the
  // result.
  const std::u16string script = base::StrCat(
      {base::UTF8ToUTF16(assets::kPageAgent), u"\n;__compassAgent.pageText({",
       u"max_chars: ", base::NumberToString16(max_chars), u"});"});
  frame->ExecuteJavaScriptInIsolatedWorld(
      script, base::BindOnce(&OnResult, std::move(callback)),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
}

}  // namespace compass::claude
