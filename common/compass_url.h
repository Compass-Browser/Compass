// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMMON_COMPASS_URL_H_
#define COMPASS_COMMON_COMPASS_URL_H_

#include <string>
#include <string_view>

class GURL;

namespace compass {

// compass:// is what the user types and sees for chrome:// pages. Pages
// never load under it: compass://x is rewritten to chrome://x before a
// navigation starts, and chrome://x is shown as compass://x.
inline constexpr char kCompassScheme[] = "compass";

// compass://x as chrome://x; any other URL unchanged.
GURL ToChromeURL(const GURL& url);

// chrome://x as compass://x; any other URL unchanged.
GURL ToCompassURL(const GURL& url);

// |text| with a leading "chrome://" (any case) written as "compass://", for
// URLs already formatted for display.
std::u16string ToCompassText(std::u16string_view text);

}  // namespace compass

#endif  // COMPASS_COMMON_COMPASS_URL_H_
