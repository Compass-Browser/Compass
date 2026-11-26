// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/webui/localized_strings/localized_strings.h"

#include "compass/strings/grit/compass_strings.h"
#include "content/public/browser/web_ui_data_source.h"
#include "ui/base/webui/web_ui_util.h"

namespace compass {

void AddEaselLocalizedStrings(content::WebUIDataSource *source) {
  static constexpr webui::LocalizedString kStrings[] = {
      {"easelAddingLink", IDS_COMPASS_EASEL_ADDING_LINK},
      {"easelAllEasels", IDS_COMPASS_EASEL_ALL_EASELS},
      {"easelBackground", IDS_COMPASS_EASEL_BACKGROUND},
      {"easelBringToFront", IDS_COMPASS_EASEL_BRING_TO_FRONT},
      {"easelCancel", IDS_COMPASS_EASEL_CANCEL},
      {"easelColorBlue", IDS_COMPASS_EASEL_COLOR_BLUE},
      {"easelColorGreen", IDS_COMPASS_EASEL_COLOR_GREEN},
      {"easelColorGrey", IDS_COMPASS_EASEL_COLOR_GREY},
      {"easelColorInk", IDS_COMPASS_EASEL_COLOR_INK},
      {"easelColorOrange", IDS_COMPASS_EASEL_COLOR_ORANGE},
      {"easelColorPurple", IDS_COMPASS_EASEL_COLOR_PURPLE},
      {"easelColorRed", IDS_COMPASS_EASEL_COLOR_RED},
      {"easelColorYellow", IDS_COMPASS_EASEL_COLOR_YELLOW},
      {"easelDelete", IDS_COMPASS_EASEL_DELETE},
      {"easelDeleteDetail", IDS_COMPASS_EASEL_DELETE_DETAIL},
      {"easelDeleteQuestion", IDS_COMPASS_EASEL_DELETE_QUESTION},
      {"easelDuplicate", IDS_COMPASS_EASEL_DUPLICATE},
      {"easelFill", IDS_COMPASS_EASEL_FILL},
      {"easelFit", IDS_COMPASS_EASEL_FIT},
      {"easelFont", IDS_COMPASS_EASEL_FONT},
      {"easelFontHand", IDS_COMPASS_EASEL_FONT_HAND},
      {"easelFontMono", IDS_COMPASS_EASEL_FONT_MONO},
      {"easelFontSans", IDS_COMPASS_EASEL_FONT_SANS},
      {"easelFontSerif", IDS_COMPASS_EASEL_FONT_SERIF},
      {"easelFull", IDS_COMPASS_EASEL_FULL},
      {"easelImageTooLarge", IDS_COMPASS_EASEL_IMAGE_TOO_LARGE},
      {"easelItemCount", IDS_COMPASS_EASEL_ITEM_COUNT},
      {"easelList", IDS_COMPASS_EASEL_LIST},
      {"easelMenuAddImage", IDS_COMPASS_EASEL_MENU_ADD_IMAGE},
      {"easelMenuAddTextHere", IDS_COMPASS_EASEL_MENU_ADD_TEXT_HERE},
      {"easelMenuBringToFront", IDS_COMPASS_EASEL_MENU_BRING_TO_FRONT},
      {"easelMenuEditText", IDS_COMPASS_EASEL_MENU_EDIT_TEXT},
      {"easelMenuRefresh", IDS_COMPASS_EASEL_MENU_REFRESH},
      {"easelMenuSendToBack", IDS_COMPASS_EASEL_MENU_SEND_TO_BACK},
      {"easelMissing", IDS_COMPASS_EASEL_MISSING},
      {"easelName", IDS_COMPASS_EASEL_NAME},
      {"easelNewEasel", IDS_COMPASS_EASEL_NEW_EASEL},
      {"easelNewerVersion", IDS_COMPASS_EASEL_NEWER_VERSION},
      {"easelNoEasels", IDS_COMPASS_EASEL_NO_EASELS},
      {"easelOpen", IDS_COMPASS_EASEL_OPEN},
      {"easelPaletteDark", IDS_COMPASS_EASEL_PALETTE_DARK},
      {"easelPaletteDots", IDS_COMPASS_EASEL_PALETTE_DOTS},
      {"easelPaletteGrid", IDS_COMPASS_EASEL_PALETTE_GRID},
      {"easelPalettePaper", IDS_COMPASS_EASEL_PALETTE_PAPER},
      {"easelRedo", IDS_COMPASS_EASEL_REDO},
      {"easelRefreshed", IDS_COMPASS_EASEL_REFRESHED},
      {"easelRefreshing", IDS_COMPASS_EASEL_REFRESHING},
      {"easelRename", IDS_COMPASS_EASEL_RENAME},
      {"easelSendToBack", IDS_COMPASS_EASEL_SEND_TO_BACK},
      {"easelShiftKey", IDS_COMPASS_EASEL_SHIFT_KEY},
      {"easelStyleBody", IDS_COMPASS_EASEL_STYLE_BODY},
      {"easelStyleCaption", IDS_COMPASS_EASEL_STYLE_CAPTION},
      {"easelStyleHeading", IDS_COMPASS_EASEL_STYLE_HEADING},
      {"easelStyleTitle", IDS_COMPASS_EASEL_STYLE_TITLE},
      {"easelTextStyle", IDS_COMPASS_EASEL_TEXT_STYLE},
      {"easelTitleLabel", IDS_COMPASS_EASEL_TITLE_LABEL},
      {"easelToolArrow", IDS_COMPASS_EASEL_TOOL_ARROW},
      {"easelToolCircle", IDS_COMPASS_EASEL_TOOL_CIRCLE},
      {"easelToolImage", IDS_COMPASS_EASEL_TOOL_IMAGE},
      {"easelToolPen", IDS_COMPASS_EASEL_TOOL_PEN},
      {"easelToolRectangle", IDS_COMPASS_EASEL_TOOL_RECTANGLE},
      {"easelToolSelect", IDS_COMPASS_EASEL_TOOL_SELECT},
      {"easelToolText", IDS_COMPASS_EASEL_TOOL_TEXT},
      {"easelUnavailable", IDS_COMPASS_EASEL_PAGE_UNAVAILABLE},
      {"easelUndo", IDS_COMPASS_EASEL_UNDO},
      {"easelUntitled", IDS_COMPASS_EASEL_PAGE_UNTITLED},
      {"easelViewOnly", IDS_COMPASS_EASEL_VIEW_ONLY},
      {"easelWithShortcut", IDS_COMPASS_EASEL_WITH_SHORTCUT},
      {"easelZoomTo100", IDS_COMPASS_EASEL_ZOOM_TO_100},
  };
  source->AddLocalizedStrings(kStrings);
}

} // namespace compass
