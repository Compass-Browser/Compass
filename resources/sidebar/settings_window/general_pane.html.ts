// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsGeneralPaneElement} from './general_pane.js';
import {PREF_HARDWARE_ACCELERATION, PREF_MINIMUM_FONT_SIZE, PREF_OFFER_TRANSLATE, PREF_PROMPT_FOR_DOWNLOAD, PREF_SPELLCHECK, PREF_FONT_SIZE} from './general_pane.js';

export function getHtml(this: CompassSettingsGeneralPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<section class="row-group">
  <h2 class="group-title">Browser</h2>
  <div class="group">
    <div class="row control-row" data-row="defaultBrowser">
      <span class="row-text">
        <span class="row-label">Default browser</span>
        <span class="row-detail">${this.getDefaultBrowserText_()}</span>
      </span>
      <button class="pill accent" ?hidden="${!this.canMakeDefault_()}"
          @click="${this.onMakeDefaultClick_}">
        Make Default
      </button>
    </div>
    <div class="row control-row" data-row="search">
      <span class="row-text">
        <span class="row-label">Search engine</span>
        <span class="row-detail">What the command bar searches with.</span>
      </span>
      <select class="popup" aria-label="Search engine"
          ?disabled="${this.isSearchEngineManaged_()}"
          @change="${this.onSearchEngineChange_}">
        ${this.engines_.map(engine => html`
          <option value="${engine.id}"
              ?selected="${engine.id === this.getDefaultEngineId_()}">
            ${engine.name}
          </option>`)}
      </select>
    </div>
    <button class="row" data-row="searchEngines" data-sub-page="searchEngines"
        @click="${this.onChromeSettingsClick_}">
      <span class="row-text">
        <span class="row-label">Manage search engines and site search</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="Opens in a tab">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Downloads</h2>
  <div class="group">
    <div class="row control-row" data-row="downloads">
      <span class="row-text">
        <span class="row-label">Location</span>
        <span class="row-detail path">${this.downloadLocation_}</span>
      </span>
      <button class="pill" @click="${this.onChangeDownloadLocationClick_}">
        Change…
      </button>
    </div>
    <label class="row control-row" data-row="promptForDownload">
      <span class="row-text">
        <span class="row-label">Ask where to save each file</span>
      </span>
      <input type="checkbox" class="switch"
          data-pref="${PREF_PROMPT_FOR_DOWNLOAD}"
          .checked="${this.getBool_(PREF_PROMPT_FOR_DOWNLOAD)}"
          ?disabled="${this.isEnforced_(PREF_PROMPT_FOR_DOWNLOAD)}"
          @change="${this.onSwitchChange_}">
    </label>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Appearance</h2>
  <div class="group">
    <div class="row control-row" data-row="fontSize">
      <span class="row-text">
        <span class="row-label">Font size</span>
      </span>
      <select class="popup" aria-label="Font size"
          data-pref="${PREF_FONT_SIZE}"
          ?disabled="${this.isEnforced_(PREF_FONT_SIZE)}"
          @change="${this.onNumberSelectChange_}">
        ${this.fontSizes.map(size => html`
          <option value="${size.value}"
              ?selected="${size.value === this.getNumber_(PREF_FONT_SIZE)}">
            ${size.label}
          </option>`)}
      </select>
    </div>
    <div class="row control-row" data-row="zoom">
      <span class="row-text">
        <span class="row-label">Page zoom</span>
        <span class="row-detail">For sites you haven't zoomed yourself.</span>
      </span>
      <select class="popup" aria-label="Page zoom"
          @change="${this.onZoomChange_}">
        ${this.getZoomFactors_().map(factor => html`
          <option value="${factor}" ?selected="${this.isZoomSelected_(factor)}">
            ${this.getZoomLabel_(factor)}
          </option>`)}
      </select>
    </div>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Fonts</h2>
  <div class="group">
    ${this.fontPrefs.map(font => html`
      <div class="row control-row" data-row="${font.pref}">
        <span class="row-text">
          <span class="row-label">${font.label}</span>
        </span>
        <select class="popup" aria-label="${font.label}"
            data-pref="${font.pref}"
            ?disabled="${this.isEnforced_(font.pref)}"
            @change="${this.onFontChange_}">
          ${this.getFontsFor_(font.pref).map(entry => html`
            <option value="${entry[0]}"
                ?selected="${entry[0] === this.getString_(font.pref)}">
              ${entry[1]}
            </option>`)}
        </select>
      </div>`)}
    <div class="row control-row" data-row="minimumFontSize">
      <span class="row-text">
        <span class="row-label">Minimum font size</span>
      </span>
      <select class="popup" aria-label="Minimum font size"
          data-pref="${PREF_MINIMUM_FONT_SIZE}"
          ?disabled="${this.isEnforced_(PREF_MINIMUM_FONT_SIZE)}"
          @change="${this.onNumberSelectChange_}">
        ${this.minimumFontSizes.map(size => html`
          <option value="${size.value}" ?selected="${
              size.value === this.getNumber_(PREF_MINIMUM_FONT_SIZE)}">
            ${size.label}
          </option>`)}
      </select>
    </div>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">Languages</h2>
  <div class="group">
    <button class="row" data-row="languages" data-sub-page="languages"
        @click="${this.onChromeSettingsClick_}">
      <span class="row-text">
        <span class="row-label">Preferred languages</span>
        <span class="row-detail">${this.getLanguagesText_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="Opens in a tab">
      </compass-settings-icon>
    </button>
    <label class="row control-row" data-row="translate">
      <span class="row-text">
        <span class="row-label">Offer to translate pages</span>
        <span class="row-detail">
          In languages that aren't among your preferred ones.
        </span>
      </span>
      <input type="checkbox" class="switch"
          data-pref="${PREF_OFFER_TRANSLATE}"
          .checked="${this.getBool_(PREF_OFFER_TRANSLATE)}"
          ?disabled="${this.isEnforced_(PREF_OFFER_TRANSLATE)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="spellcheck">
      <span class="row-text">
        <span class="row-label">Check spelling as you type</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_SPELLCHECK}"
          .checked="${this.getBool_(PREF_SPELLCHECK)}"
          ?disabled="${this.isEnforced_(PREF_SPELLCHECK)}"
          @change="${this.onSwitchChange_}">
    </label>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">System</h2>
  <div class="group">
    <div class="row control-row" data-row="hardwareAcceleration">
      <span class="row-text">
        <span class="row-label">Use graphics acceleration when available</span>
        <span class="row-detail" ?hidden="${!this.needsRelaunch_()}">
          Takes effect when Compass restarts.
        </span>
      </span>
      <button class="pill accent" ?hidden="${!this.needsRelaunch_()}"
          @click="${this.onRelaunchClick_}">
        Relaunch
      </button>
      <input type="checkbox" class="switch"
          aria-label="Use graphics acceleration when available"
          data-pref="${PREF_HARDWARE_ACCELERATION}"
          .checked="${this.getBool_(PREF_HARDWARE_ACCELERATION)}"
          ?disabled="${this.isEnforced_(PREF_HARDWARE_ACCELERATION)}"
          @change="${this.onSwitchChange_}">
    </div>
    <button class="row" data-row="proxy" @click="${this.onProxyClick_}">
      <span class="row-text">
        <span class="row-label">Proxy settings</span>
        <span class="row-detail">In your computer's network settings.</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="Opens your computer's settings">
      </compass-settings-icon>
    </button>
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
