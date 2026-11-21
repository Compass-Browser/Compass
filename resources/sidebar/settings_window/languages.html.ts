// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsLanguagesElement} from './languages.js';

export function getHtml(this: CompassSettingsLanguagesElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  General
</button>

<section class="row-group">
  <h2 class="group-title">Preferred languages</h2>
  <div class="group">
    ${this.preferred_.map((language, index) => html`
      <div class="row control-row" data-language="${language.code}">
        <span class="row-text">
          <span class="row-label">${this.getName_(language.code)}</span>
          <span class="row-detail">${this.getNativeName_(language.code)}</span>
        </span>
        <label class="inline-switch"
            ?hidden="${!this.supportsTranslate_(language.code)}">
          Translate
          <input type="checkbox" class="switch" data-code="${language.code}"
              aria-label="Offer to translate ${this.getName_(language.code)}"
              .checked="${language.translateOffered}"
              @change="${this.onTranslateChange_}">
        </label>
        <button class="pill small" data-code="${language.code}"
            aria-label="Move ${this.getName_(language.code)} up"
            ?disabled="${this.isFirst_(index)}" @click="${this.onMoveUpClick_}">
          ↑
        </button>
        <button class="pill small" data-code="${language.code}"
            aria-label="Move ${this.getName_(language.code)} down"
            ?disabled="${this.isLast_(index)}"
            @click="${this.onMoveDownClick_}">
          ↓
        </button>
        <button class="pill small" data-code="${language.code}"
            aria-label="Remove ${this.getName_(language.code)}"
            ?disabled="${!this.canRemove_()}" @click="${this.onRemoveClick_}">
          Remove
        </button>
      </div>`)}
  </div>
</section>

<section class="row-group">
  <div class="group-header">
    <h2 class="group-title">Add a language</h2>
    <input class="field filter" type="search" placeholder="Filter"
        aria-label="Filter languages" spellcheck="false"
        .value="${this.filter_}" @input="${this.onFilterInput_}">
  </div>
  <div class="group scrolling-group">
    ${this.getAddable_().map(language => html`
      <div class="row control-row">
        <span class="row-text">
          <span class="row-label">${language.name}</span>
          <span class="row-detail">${language.nativeName}</span>
        </span>
        <button class="pill small" data-code="${language.code}"
            aria-label="Add ${language.name}" @click="${this.onAddClick_}">
          Add
        </button>
      </div>`)}
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
