// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/common/compass_url.h"

#include "base/strings/string_util.h"
#include "url/gurl.h"

namespace compass {

namespace {

// content::kChromeUIScheme, without depending on //content from every
// process's common code.
constexpr char kChromeScheme[] = "chrome";

GURL WithScheme(const GURL& url, std::string_view scheme) {
  GURL::Replacements replacements;
  replacements.SetSchemeStr(scheme);
  return url.ReplaceComponents(replacements);
}

}  // namespace

GURL ToChromeURL(const GURL& url) {
  return url.SchemeIs(kCompassScheme) ? WithScheme(url, kChromeScheme) : url;
}

GURL ToCompassURL(const GURL& url) {
  return url.SchemeIs(kChromeScheme) ? WithScheme(url, kCompassScheme) : url;
}

std::u16string ToCompassText(std::u16string_view text) {
  constexpr std::u16string_view kChromePrefix = u"chrome://";
  if (!base::StartsWith(text, kChromePrefix,
                        base::CompareCase::INSENSITIVE_ASCII)) {
    return std::u16string(text);
  }
  return u"compass://" + std::u16string(text.substr(kChromePrefix.size()));
}

}  // namespace compass
