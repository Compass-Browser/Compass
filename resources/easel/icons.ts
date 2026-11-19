// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {svg} from './dom.js';

// The toolbar's icons, 20 x 20, stroked with the text color.
const PATHS: Record<string, string[]> = {
  'select': ['M5 3l10 6.5-4.5 1.2 2.6 4.8-2 1-2.6-4.8L5 15z'],
  'text': ['M4 5V3.5h12V5', 'M10 3.5V17', 'M7.5 17h5'],
  'rectangle': ['M3.5 5h13v10h-13z'],
  'circle': ['M10 3.5a6.5 6.5 0 1 0 0.01 0z'],
  'arrow': ['M4 16L16 4', 'M9 4h7v7'],
  'pen': ['M3 17c3-1 4-6 7-6s2 4 4 4 2.5-3 3-5'],
  'image': ['M3 4.5h14v11H3z', 'M3 13l4-4 3.5 3.5 2.5-2.5 4 4', 'M13 7.5h.01'],
  'undo': ['M7 5L3.5 8.5 7 12', 'M3.5 8.5H12a4.5 4.5 0 0 1 0 9H9'],
  'redo': ['M13 5l3.5 3.5L13 12', 'M16.5 8.5H8a4.5 4.5 0 0 0 0 9h3'],
  'front': ['M7 7h9v9H7z', 'M4 13V4h9'],
  'back': ['M4 4h9v9H4z', 'M7 16h9V7'],
  'duplicate': ['M7 7h9v9H7z', 'M13 4H4v9'],
  'delete': ['M4 6h12', 'M8 6V4h4v2', 'M5.5 6l1 10.5h7l1-10.5'],
  'fit': ['M3 7V3h4', 'M13 3h4v4', 'M17 13v4h-4', 'M7 17H3v-4'],
  'fill': ['M3.5 5h13v10h-13z', 'M3.5 9l6-4', 'M3.5 14l11-9', 'M8 15l8.5-6'],
  'more': ['M5 10h.01', 'M10 10h.01', 'M15 10h.01'],
};

export function icon(name: string): SVGSVGElement {
  const paths = (PATHS[name] || []).map(d => svg('path', {d}));
  return svg(
      'svg', {
        'class': 'icon',
        'viewBox': '0 0 20 20',
        'width': 20,
        'height': 20,
        'aria-hidden': 'true',
      },
      paths);
}
