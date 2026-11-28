// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Pages' languages come from CLD3, built into Chromium, not from upstream's
// TFLite model, which Optimization Guide downloads only with Google's API
// keys; without it Chromium believed the page's lang attribute alone
// (docs/design/translation.md, "Page language").

#include "components/translate/core/common/translate_util.h"

#define IsTFLiteLanguageDetectionEnabled \
  IsTFLiteLanguageDetectionEnabled_ChromiumImpl
#include "src/components/translate/core/common/translate_util.cc"
#undef IsTFLiteLanguageDetectionEnabled

namespace translate {

bool IsTFLiteLanguageDetectionEnabled() {
  return false;
}

}  // namespace translate
