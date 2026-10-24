// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {PlaceholderHandler} from './sidebar.mojom-webui.js';

// chrome://strata-placeholder: names the tab that moved to another window and
// brings it back on request. See docs/design/sidebar.md, "Window sync".
const handler = PlaceholderHandler.getRemote();

handler.getInfo().then(({title, url}) => {
  const name = title || url || 'This tab';
  document.querySelector('#title')!.textContent = name;
  document.title = name;
});

document.querySelector('#bringHere')!.addEventListener(
    'click', () => handler.bringHere());
