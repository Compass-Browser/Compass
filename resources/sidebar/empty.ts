// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import {EmptyPageHandler} from './sidebar.mojom-webui.js';

// chrome://compass-empty: what a window with nothing else to show shows. There
// is no new tab page: the command bar opens whenever this page comes into view,
// and on a click. See docs/design/command-bar.md, "No new tab page".
const handler = EmptyPageHandler.getRemote();

// Follows the window's theme, which follows its space.
ColorChangeUpdater.forDocument().start();

function showCommandBarIfVisible() {
  if (document.visibilityState === 'visible') {
    handler.showCommandBar();
  }
}

document.addEventListener('visibilitychange', showCommandBarIfVisible);
document.addEventListener('click', () => handler.showCommandBar());
showCommandBarIfVisible();
