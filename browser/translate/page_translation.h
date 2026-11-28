// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_TRANSLATE_PAGE_TRANSLATION_H_
#define COMPASS_BROWSER_TRANSLATE_PAGE_TRANSLATION_H_

#include <optional>
#include <string>
#include <string_view>

// What upstream's page translation calls into (docs/design/translation.md,
// "Desktop: the page"). Only standard types here: chromium_src overrides
// of components/translate include this.

namespace compass::translate {

// What Chromium injects into a page's translate world after its
// translate.js, in place of Google's library: page_translator.js and
// desktop_service.js (chromium_src/components/translate/core/browser/
// translate_script.cc).
std::string PageTranslateScript();

// Whether Compass translates pages in `language` (a page or translate-UI
// language code): the languages it has models for (chromium_src/.../
// translate_language_list.cc).
bool CanTranslateLanguage(std::string_view language);

// Upstream's on-device code for `language` ("iw", "no", "zh", "zh-Hant"),
// or nullopt.
std::optional<std::string> ToOnDeviceLanguage(std::string_view language);

// Turns upstream's page translation on without Google's API keys. Early in
// the browser process.
void EnablePageTranslation();

}  // namespace compass::translate

#endif  // COMPASS_BROWSER_TRANSLATE_PAGE_TRANSLATION_H_
