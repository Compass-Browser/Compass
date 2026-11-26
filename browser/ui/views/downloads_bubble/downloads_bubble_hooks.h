// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_DOWNLOADS_BUBBLE_HOOKS_H_
#define COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_DOWNLOADS_BUBBLE_HOOKS_H_

class BrowserView;
class BrowserWindowInterface;

namespace views {
class ScrollView;
class View;
}  // namespace views

// What upstream's downloads bubble calls (docs/design/downloads-bubble.md;
// see patches/chrome-browser-ui-views-download-bubble-*.patch). Declared
// with no dependencies so upstream's download target can include it; defined
// in //compass/browser/ui.
namespace compass::downloads_bubble {

// The width of the full downloads bubble's rows: wider than upstream's, for
// the file names and three screenshots a row. The bubble adds its cards'
// margins.
int DialogWidth();

// Turns upstream's full downloads bubble (DownloadDialogView: a header, its
// rows in |rows|, a footer) into the tabbed one: Downloads (its rows, then
// the download folder's other files) and Screenshots.
void ExtendDownloadDialog(views::View* dialog,
                          views::ScrollView* rows,
                          BrowserWindowInterface* browser);

// The downloads button was pressed with no recent downloads, where upstream
// opens chrome://downloads: shows the tabbed bubble without upstream's rows
// instead (or closes it, if it's open). False if it can't (no button to
// anchor to), so upstream goes on as before.
bool ShowWithoutRecentDownloads(BrowserView* browser_view);

}  // namespace compass::downloads_bubble

#endif  // COMPASS_BROWSER_UI_VIEWS_DOWNLOADS_BUBBLE_DOWNLOADS_BUBBLE_HOOKS_H_
