// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The languages page translation offers are the ones Compass has models
// for, and the list is never fetched from Google (docs/design/
// translation.md, "Languages"): Chromium's built-in list, filtered, and a
// fetcher that never fetches.

#include <memory>
#include <string>
#include <vector>

#include "base/i18n/language_tag.h"
#include "build/build_config.h"
#include "components/translate/core/browser/translate_url_fetcher.h"
// Declare TranslateURLFetcherImpl and GetDefaultSupportedLanguages under
// their own names, before the renaming.
#include "components/translate/core/common/translate_language_matcher.h"

#if !BUILDFLAG(IS_IOS) && !BUILDFLAG(IS_ANDROID)
#include "compass/browser/translate/page_translation.h"

namespace translate {
namespace {

// Already complete: TranslateLanguageList never asks it for anything.
class CompassNoLanguageListFetcher : public TranslateUrlFetcher {
 public:
  explicit CompassNoLanguageListFetcher(int) {}
  bool Request(const GURL&, Callback, bool) override { return false; }
  State state() const override { return COMPLETED; }
};

std::vector<base::i18n::LanguageTag> CompassSupportedLanguages() {
  std::vector<base::i18n::LanguageTag> languages;
  for (const base::i18n::LanguageTag& tag : GetDefaultSupportedLanguages()) {
    if (compass::translate::CanTranslateLanguage(tag.tag_string())) {
      languages.push_back(tag);
    }
  }
  return languages;
}

}  // namespace
}  // namespace translate

#define TranslateURLFetcherImpl CompassNoLanguageListFetcher
#define GetDefaultSupportedLanguages CompassSupportedLanguages
#endif  // !BUILDFLAG(IS_IOS) && !BUILDFLAG(IS_ANDROID)

#include "src/components/translate/core/browser/translate_language_list.cc"

#undef GetDefaultSupportedLanguages
#undef TranslateURLFetcherImpl
