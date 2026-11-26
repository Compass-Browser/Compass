// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_WEBUI_LOCALIZED_STRINGS_LOCALIZED_STRINGS_H_
#define COMPASS_BROWSER_UI_WEBUI_LOCALIZED_STRINGS_LOCALIZED_STRINGS_H_

namespace content {
class WebUIDataSource;
}

namespace compass {

// Compass's WebUI strings (//compass/strings:compass_strings), by area, for
// loadTimeData (resources/sidebar/i18n.ts) and $i18n{} in the settings
// sections' markup. Keys start with their area's name so the areas can share
// a data source.

// Buttons and words every area uses ("cancel", "save").
void AddCommonLocalizedStrings(content::WebUIDataSource *source);
// The settings window's sections (resources/sidebar/settings/).
void AddSettingsSectionsLocalizedStrings(content::WebUIDataSource *source);
// The settings window (resources/sidebar/settings_window/).
void AddSettingsWindowLocalizedStrings(content::WebUIDataSource *source);
// The sidebar and the pages around it: the command bar, Claude's chat, the
// space editor, sharing, live folders, the find bar, the placeholder
// (the rest of resources/sidebar/).
void AddSidebarLocalizedStrings(content::WebUIDataSource *source);

// chrome://easel (resources/easel/), on the desktop and Android.
void AddEaselLocalizedStrings(content::WebUIDataSource *source);

// Every area of the sidebar's pak (resources/sidebar/), which all of its
// pages get: their modules are shared, so a page can show any of them.
void AddSidebarPakLocalizedStrings(content::WebUIDataSource *source);

} // namespace compass

#endif // COMPASS_BROWSER_UI_WEBUI_LOCALIZED_STRINGS_LOCALIZED_STRINGS_H_
