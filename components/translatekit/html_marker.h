// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_TRANSLATEKIT_HTML_MARKER_H_
#define COMPASS_COMPONENTS_TRANSLATEKIT_HTML_MARKER_H_

#include <string_view>

namespace compass::translatekit {

// Text sent to TranslatorTranslate() that starts with this is HTML: the
// marker is dropped, the rest translated with its tags kept. It lets page
// translation ask for HTML through upstream's service and mojo interfaces
// unchanged (they only carry a string), instead of patching each of them.
//
// U+FDD0 U+FDD1, Unicode noncharacters, which are for a program's internal
// use. A web page sending them through the Translator API only gets its own
// text translated as HTML.
inline constexpr std::string_view kHtmlMarker = "\xEF\xB7\x90\xEF\xB7\x91";

}  // namespace compass::translatekit

#endif  // COMPASS_COMPONENTS_TRANSLATEKIT_HTML_MARKER_H_
