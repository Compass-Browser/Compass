// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import {PlaceholderHandler} from './sidebar.mojom-webui.js';

// chrome://compass-placeholder: a frozen screenshot of the tab that moved to
// another window, with a way to bring it back. The window also brings it back
// on its own when it's focused. See docs/design/sidebar.md, "Window sync".
const handler = PlaceholderHandler.getRemote();

// Follows the window's theme, which follows its space.
ColorChangeUpdater.forDocument().start();

handler.getInfo().then(({title, url}) => {
  const name = title || url || 'This tab';
  document.querySelector('#title')!.textContent = name;
  document.title = name;
});

handler.getSnapshot().then(({dataUrl}) => {
  if (dataUrl) {
    document.querySelector<HTMLImageElement>('#snapshot')!.src = dataUrl;
    document.body.classList.add('has-snapshot');
  }
});

document.querySelector('#bringHere')!.addEventListener(
    'click', () => handler.bringHere());
