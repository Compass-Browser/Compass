// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/web_store/web_store_branding.h"

#include <string>

#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace compass {

namespace {

constexpr char kWebStoreHost[] = "chromewebstore.google.com";

// Runs in an isolated world: it shares the page's DOM, not its JavaScript.
// The store renders the button's label as a text node in a <span> in a
// <button> and swaps it as the state changes (Add, Adding, Remove), so the
// observer renames whatever lands in a button. "Chrome" stays in Latin
// letters in every language the store speaks, as a whole word.
constexpr char16_t kScript[] = uR"js((() => {
  if (window.compassWebStoreBranding) {
    return;
  }
  window.compassWebStoreBranding = true;
  const NAME = /\bChrome\b/g;
  const rename = (button) => {
    const walker = document.createTreeWalker(button, NodeFilter.SHOW_TEXT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      if (NAME.test(node.data)) {
        node.data = node.data.replace(NAME, 'Compass');
      }
      NAME.lastIndex = 0;
    }
    for (const attribute of ['aria-label', 'title']) {
      const value = button.getAttribute(attribute);
      if (value && NAME.test(value)) {
        button.setAttribute(attribute, value.replace(NAME, 'Compass'));
      }
      NAME.lastIndex = 0;
    }
  };
  const renameIn = (node) => {
    const element = node.nodeType === Node.ELEMENT_NODE ? node
                                                        : node.parentElement;
    if (!element) {
      return;
    }
    const button = element.closest('button');
    if (button) {
      rename(button);
      return;
    }
    if (node.nodeType === Node.ELEMENT_NODE) {
      node.querySelectorAll('button').forEach(rename);
    }
  };
  renameIn(document.documentElement);
  new MutationObserver((records) => {
    for (const record of records) {
      if (record.type === 'childList') {
        record.addedNodes.forEach(renameIn);
      } else {
        renameIn(record.target);
      }
    }
  }).observe(document.documentElement, {
    subtree: true,
    childList: true,
    characterData: true,
    attributes: true,
    attributeFilter: ['aria-label', 'title'],
  });
})();)js";

class WebStoreBranding
    : public content::WebContentsObserver,
      public content::WebContentsUserData<WebStoreBranding> {
 public:
  ~WebStoreBranding() override = default;

  // content::WebContentsObserver:
  void DOMContentLoaded(content::RenderFrameHost* frame) override {
    if (!frame->IsInPrimaryMainFrame() ||
        !IsWebStoreURL(frame->GetLastCommittedURL())) {
      return;
    }
    frame->ExecuteJavaScriptInIsolatedWorld(
        std::u16string(kScript), base::NullCallback(),
        ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }

 private:
  friend class content::WebContentsUserData<WebStoreBranding>;

  explicit WebStoreBranding(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        content::WebContentsUserData<WebStoreBranding>(*web_contents) {}

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(WebStoreBranding);

}  // namespace

void AttachWebStoreBranding(content::WebContents* web_contents) {
  WebStoreBranding::CreateForWebContents(web_contents);
}

bool IsWebStoreURL(const GURL& url) {
  return url.SchemeIs(url::kHttpsScheme) && url.host() == kWebStoreHost;
}

std::u16string WebStoreBrandingScriptForTesting() {
  return std::u16string(kScript);
}

}  // namespace compass
