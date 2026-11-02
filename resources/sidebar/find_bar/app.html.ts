// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassFindBarAppElement} from './app.js';

export function getHtml(this: CompassFindBarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="bar" role="search">
  <svg class="search" viewBox="0 0 16 16" aria-hidden="true">
    <circle cx="7" cy="7" r="4.5"></circle>
    <line x1="10.4" y1="10.4" x2="13.5" y2="13.5"></line>
  </svg>
  <input id="input" type="text" spellcheck="false" autocomplete="off"
      aria-label="Find in page" placeholder="Find in page"
      aria-describedby="count"
      @input="${this.onInput_}" @keydown="${this.onKeydown_}"
      @compositionend="${this.onCompositionEnd_}">
  <span id="count" role="status" ?no-match="${this.isNoMatch_()}"
      aria-live="polite">${this.getCountText_()}</span>
  <span class="divider" aria-hidden="true"></span>
  <button id="previous" class="icon" title="Previous match (⇧⏎)"
      aria-label="Previous match" ?disabled="${!this.hasMatches_()}"
      @click="${this.onPreviousClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <polyline points="4,10 8,6 12,10"></polyline>
    </svg>
  </button>
  <button id="next" class="icon" title="Next match (⏎)"
      aria-label="Next match" ?disabled="${!this.hasMatches_()}"
      @click="${this.onNextClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <polyline points="4,6 8,10 12,6"></polyline>
    </svg>
  </button>
  <button id="close" class="icon" title="Close (Esc)" aria-label="Close"
      @click="${this.onCloseClick_}">
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <line x1="4.5" y1="4.5" x2="11.5" y2="11.5"></line>
      <line x1="11.5" y1="4.5" x2="4.5" y2="11.5"></line>
    </svg>
  </button>
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
