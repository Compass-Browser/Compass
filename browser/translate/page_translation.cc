// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/translate/page_translation.h"

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "compass/browser/translate/page_translate_scripts.h"
#include "components/on_device_translation/public/supported_languages.h"
#include "components/translate/core/browser/translate_manager.h"

namespace compass::translate {

std::string PageTranslateScript() {
  return base::StrCat({kPageTranslatorJs, "\n", kDesktopServiceJs});
}

std::optional<std::string> ToOnDeviceLanguage(std::string_view language) {
  const std::string code = base::ToLowerASCII(language);
  std::string result;
  if (code == "zh-tw" || code == "zh-hk" || code == "zh-hant" ||
      code == "zh-mo") {
    result = "zh-Hant";
  } else if (code == "zh" || base::StartsWith(code, "zh-")) {
    result = "zh";
  } else if (code == "he" || code == "iw") {
    result = "iw";
  } else if (code == "nb" || code == "no" || code == "nn") {
    result = "no";
  } else {
    result = code.substr(0, code.find('-'));
  }
  if (!on_device_translation::ToSupportedLanguage(result)) {
    return std::nullopt;
  }
  return result;
}

bool CanTranslateLanguage(std::string_view language) {
  return ToOnDeviceLanguage(language).has_value();
}

void EnablePageTranslation() {
  // Upstream turns page translation off without Google's API keys (the
  // bubble, the menu, its availability). Compass translates on the device
  // with neither: this switch, upstream's own for tests, skips exactly
  // those checks.
  ::translate::TranslateManager::SetIgnoreMissingKeyForTesting(true);
}

}  // namespace compass::translate
