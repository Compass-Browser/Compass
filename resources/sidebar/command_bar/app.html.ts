// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {Result} from '../command_bar.mojom-webui.js';

import type {CompassCommandBarAppElement} from './app.js';

export function getHtml(this: CompassCommandBarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="field">
  <span class="glyph" aria-hidden="true">⌕</span>
  <input id="input" type="text" spellcheck="false" autocomplete="off"
      aria-label="Command bar" aria-controls="results"
      placeholder="${this.getPlaceholder_()}"
      @input="${this.onInput_}" @keydown="${this.onKeydown_}">
</div>
${this.results_.length ? html`
  <div id="results" role="listbox"
      @pointermove="${this.onResultPointermove_}"
      @click="${this.onResultClick_}">
    ${this.results_.map((result: Result, index: number) => html`
      <div class="result" role="option" data-index="${index}"
          ?selected="${this.isSelected_(index)}"
          aria-selected="${this.isSelected_(index)}">
        ${this.hasFavicon_(result) ? html`
          <img class="icon" src="${this.getFaviconUrl_(result)}" alt="">` :
          html`<span class="icon glyph" aria-hidden="true">
            ${this.getGlyph_(result)}</span>`}
        <span class="title">${result.title}</span>
        ${result.subtitle ? html`
          <span class="subtitle">${result.subtitle}</span>` : nothing}
        <span class="action">${result.action}</span>
      </div>`)}
  </div>` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
