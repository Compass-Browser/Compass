// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/page_reader.h"

#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_writer.h"
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
using AgentCallback =
    base::OnceCallback<void(base::expected<base::DictValue, std::string>)>;

void OnAgentResult(AgentCallback callback, base::Value result) {
  base::DictValue* dict = result.GetIfDict();
  if (!dict) {
    std::move(callback).Run(
        base::unexpected("The page didn't answer (it may still be "
                         "loading)."));
    return;
  }
  if (!dict->FindBool("ok").value_or(false)) {
    const std::string* error = dict->FindString("error");
    std::move(callback).Run(base::unexpected(
        error ? *error : std::string("The page couldn't be read.")));
    return;
  }
  dict->Remove("ok");
  std::move(callback).Run(std::move(*dict));
}

void OnPageText(Callback callback,
                base::expected<base::DictValue, std::string> result) {
  if (!result.has_value()) {
    std::move(callback).Run(base::unexpected(result.error()));
    return;
  }
  PageText page;
  if (const std::string* url = result->FindString("url")) {
    page.url = *url;
  }
  if (const std::string* title = result->FindString("title")) {
    page.title = *title;
  }
  if (const std::string* text = result->FindString("text")) {
    page.text = *text;
  }
  page.truncated = result->FindBool("truncated").value_or(false);
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

void RunPageAgent(content::WebContents* contents,
                  const std::string& method,
                  base::DictValue opts,
                  AgentCallback callback) {
  content::RenderFrameHost* frame = contents->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    std::move(callback).Run(base::unexpected("The page isn't loaded yet."));
    return;
  }
  std::string json;
  base::JSONWriter::Write(opts, &json);
  // The agent installs itself once per document; the last expression is the
  // result.
  const std::u16string script = base::StrCat(
      {base::UTF8ToUTF16(assets::kPageAgent), u"\n;__compassAgent.",
       base::UTF8ToUTF16(method), u"(", base::UTF8ToUTF16(json), u");"});
  frame->ExecuteJavaScriptInIsolatedWorld(
      script, base::BindOnce(&OnAgentResult, std::move(callback)),
      ISOLATED_WORLD_ID_CHROME_INTERNAL);
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
  RunPageAgent(contents, "pageText",
               base::DictValue().Set("max_chars", max_chars),
               base::BindOnce(&OnPageText, std::move(callback)));
}

}  // namespace compass::claude
