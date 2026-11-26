// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/localized_strings/localized_strings.h"

#include "compass/strings/grit/compass_strings.h"
#include "content/public/browser/web_ui_data_source.h"
#include "ui/base/webui/web_ui_util.h"

namespace compass {

void AddCommonLocalizedStrings(content::WebUIDataSource *source) {
  static constexpr webui::LocalizedString kStrings[] = {
      {"add", IDS_COMPASS_ADD},           {"back", IDS_COMPASS_BACK},
      {"cancel", IDS_COMPASS_CANCEL},     {"close", IDS_COMPASS_CLOSE},
      {"continue", IDS_COMPASS_CONTINUE}, {"copy", IDS_COMPASS_COPY},
      {"delete", IDS_COMPASS_DELETE},     {"done", IDS_COMPASS_DONE},
      {"edit", IDS_COMPASS_EDIT},         {"open", IDS_COMPASS_OPEN},
      {"remove", IDS_COMPASS_REMOVE},     {"rename", IDS_COMPASS_RENAME},
      {"save", IDS_COMPASS_SAVE},         {"tryAgain", IDS_COMPASS_TRY_AGAIN},
  };
  source->AddLocalizedStrings(kStrings);
}

void AddSidebarPakLocalizedStrings(content::WebUIDataSource *source) {
  AddCommonLocalizedStrings(source);
  AddSettingsSectionsLocalizedStrings(source);
  AddSettingsWindowLocalizedStrings(source);
  AddSidebarLocalizedStrings(source);
}

} // namespace compass
