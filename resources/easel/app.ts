// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// chrome://easel/ is the list; chrome://easel/<uuid> a board's editor.

import {Editor} from './editor.js';
import {EaselList} from './list.js';

const UUID =
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function main() {
  const root = document.querySelector<HTMLElement>('#app');
  if (!root) {
    return;
  }
  const path = location.pathname.replace(/^\/+|\/+$/g, '').toLowerCase();
  if (UUID.test(path)) {
    void new Editor(root, path).start();
  } else {
    void new EaselList(root).start();
  }
}

main();
