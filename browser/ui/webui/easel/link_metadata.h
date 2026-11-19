// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_EASEL_LINK_METADATA_H_
#define COMPASS_BROWSER_UI_WEBUI_EASEL_LINK_METADATA_H_

#include <string>
#include <string_view>

#include "url/gurl.h"

namespace compass::easel {

// What a link card shows of a page (docs/design/easels.md, "link").
struct LinkMetadata {
  std::string title;
  std::string description;
  // http or https; empty if the page has none.
  GURL image_url;
};

// Reads a page's head: og:title (else twitter:title, else <title>),
// og:description (else twitter:description, else the description meta),
// og:image (else twitter:image), relative to |page_url|. A small tag
// scanner, not a full HTML parser: it stops at <body>, and ignores
// comments and scripts. Text is decoded (entities) and its whitespace
// collapsed.
LinkMetadata ParseLinkMetadata(std::string_view html, const GURL& page_url);

// The charset the head's <meta charset> or <meta http-equiv content-type>
// names, lowercase; "" if none.
std::string FindMetaCharset(std::string_view html);

// Decodes &amp;, &lt;, &#39;, &#x27; ... (the common named ones and every
// numeric one); unknown entities stay as written.
std::string DecodeHtmlEntities(std::string_view text);

}  // namespace compass::easel

#endif  // COMPASS_BROWSER_UI_WEBUI_EASEL_LINK_METADATA_H_
