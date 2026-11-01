// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

// A speaker, with sound waves, or crossed out when |muted|. Tab rows and
// favorites' tiles show it on tabs that play sound. Whole <svg>s: lit can't
// put a nested template's paths in the SVG namespace.
export function speakerIcon(muted: boolean) {
  // clang-format off
  return muted ? html`
    <svg class="speaker-icon" viewBox="0 0 16 16" aria-hidden="true">
      <path class="speaker-body" d="M2.5 6h2.5l3.5-3v10l-3.5-3h-2.5z"></path>
      <path class="speaker-line" d="M11 6l4 4M15 6l-4 4"></path>
    </svg>` : html`
    <svg class="speaker-icon" viewBox="0 0 16 16" aria-hidden="true">
      <path class="speaker-body" d="M2.5 6h2.5l3.5-3v10l-3.5-3h-2.5z"></path>
      <path class="speaker-line" d="M10.75 5.75a3 3 0 0 1 0 4.5"></path>
      <path class="speaker-line" d="M12.5 3.75a6 6 0 0 1 0 8.5"></path>
    </svg>`;
  // clang-format on
}
