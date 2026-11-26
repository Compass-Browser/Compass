// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n} from '../i18n.js';

import type {CompassSettingsGeneralPaneElement} from './general_pane.js';
import {PREF_HARDWARE_ACCELERATION, PREF_PIP_ON_LEAVING_WINDOW, PREF_MINIMUM_FONT_SIZE, PREF_OFFER_TRANSLATE, PREF_PROMPT_FOR_DOWNLOAD, PREF_SPELLCHECK, PREF_FONT_SIZE} from './general_pane.js';

export function getHtml(this: CompassSettingsGeneralPaneElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isView_('engines') ? html`
  <compass-settings-search-engines @close="${this.onEnginesClose_}">
  </compass-settings-search-engines>` : nothing}
${this.isView_('languages') ? html`
  <compass-settings-languages @close="${this.onLanguagesClose_}">
  </compass-settings-languages>` : nothing}
${this.isView_('fonts') ? html`
<button class="back" @click="${this.onFontsBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  ${i18n('generalPaneTitle')}
</button>
<h2 class="subpage-title">${i18n('generalPaneFonts')}</h2>
<section class="row-group">
  <div class="group">
    ${this.fontPrefs.map(font => html`
      <div class="row control-row" data-row="${font.pref}">
        <span class="tile" data-color="purple">
          <compass-settings-icon name="font"></compass-settings-icon>
        </span>
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
      <span class="tile" data-color="purple">
        <compass-settings-icon name="textsize"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneMinimumFontSize')}</span>
      </span>
      <select class="popup" aria-label="${i18n('generalPaneMinimumFontSize')}"
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

` : nothing}
<div ?hidden="${!this.isView_('main')}">
<section class="row-group">
  <h2 class="group-title">${i18n('generalPaneBrowser')}</h2>
  <div class="group">
    <div class="row control-row" data-row="defaultBrowser">
      <span class="tile" data-color="blue">
        <compass-settings-icon name="compass"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneDefaultBrowser')}</span>
        <span class="row-detail">${this.getDefaultBrowserText_()}</span>
      </span>
      <button class="pill accent" ?hidden="${!this.canMakeDefault_()}"
          @click="${this.onMakeDefaultClick_}">
        ${i18n('generalPaneMakeDefault')}
      </button>
    </div>
    <div class="row control-row" data-row="search">
      <span class="tile" data-color="indigo">
        <compass-settings-icon name="search"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneSearchEngine')}</span>
        <span class="row-detail">${i18n('generalPaneSearchEngineDetail')}</span>
      </span>
      <select class="popup" aria-label="${i18n('generalPaneSearchEngine')}"
          ?disabled="${this.isSearchEngineManaged_()}"
          @change="${this.onSearchEngineChange_}">
        ${this.engines_.map(engine => html`
          <option value="${engine.id}"
              ?selected="${engine.id === this.getDefaultEngineId_()}">
            ${engine.name}
          </option>`)}
      </select>
    </div>
    <button class="row" data-row="searchEngines"
        @click="${this.onManageEnginesClick_}">
      <span class="tile" data-color="gray">
        <compass-settings-icon name="search"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneManageSearchEngines')}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">${i18n('generalPanePip')}</h2>
  <div class="group">
    <label class="row control-row" data-row="pipOnLeavingWindow">
      <span class="tile" data-color="pink">
        <compass-settings-icon name="pip"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPanePipOnLeavingWindow')}</span>
        <span class="row-detail">
          ${i18n('generalPanePipOnLeavingWindowDetail')}
        </span>
      </span>
      <input type="checkbox" class="switch"
          data-pref="${PREF_PIP_ON_LEAVING_WINDOW}"
          .checked="${this.getBool_(PREF_PIP_ON_LEAVING_WINDOW)}"
          ?disabled="${this.isEnforced_(PREF_PIP_ON_LEAVING_WINDOW)}"
          @change="${this.onSwitchChange_}">
    </label>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">${i18n('generalPaneDownloads')}</h2>
  <div class="group">
    <div class="row control-row" data-row="downloads">
      <span class="tile" data-color="blue">
        <compass-settings-icon name="download"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneDownloadLocation')}</span>
        <span class="row-detail path">${this.downloadLocation_}</span>
      </span>
      <button class="pill" @click="${this.onChangeDownloadLocationClick_}">
        ${i18n('generalPaneChangeEllipsis')}
      </button>
    </div>
    <label class="row control-row" data-row="promptForDownload">
      <span class="tile" data-color="teal">
        <compass-settings-icon name="folder"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPanePromptForDownload')}</span>
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
  <h2 class="group-title">${i18n('generalPaneAppearance')}</h2>
  <div class="group">
    <div class="row control-row" data-row="fontSize">
      <span class="tile" data-color="purple">
        <compass-settings-icon name="textsize"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneFontSize')}</span>
      </span>
      <select class="popup" aria-label="${i18n('generalPaneFontSize')}"
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
      <span class="tile" data-color="purple">
        <compass-settings-icon name="zoom"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPanePageZoom')}</span>
        <span class="row-detail">${i18n('generalPanePageZoomDetail')}</span>
      </span>
      <select class="popup" aria-label="${i18n('generalPanePageZoom')}"
          @change="${this.onZoomChange_}">
        ${this.getZoomFactors_().map(factor => html`
          <option value="${factor}" ?selected="${this.isZoomSelected_(factor)}">
            ${this.getZoomLabel_(factor)}
          </option>`)}
      </select>
    </div>
    <button class="row" data-row="fonts" @click="${this.onFontsClick_}">
      <span class="tile" data-color="purple">
        <compass-settings-icon name="font"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneFonts')}</span>
        <span class="row-detail">${this.getFontsSummary_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">${i18n('generalPaneLanguages')}</h2>
  <div class="group">
    <button class="row" data-row="languages"
        @click="${this.onManageLanguagesClick_}">
      <span class="tile" data-color="teal">
        <compass-settings-icon name="globe"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPanePreferredLanguages')}</span>
        <span class="row-detail">${this.getLanguagesText_()}</span>
      </span>
      <compass-settings-icon class="row-trail" name="chevron">
      </compass-settings-icon>
    </button>
    <label class="row control-row" data-row="translate">
      <span class="tile" data-color="teal">
        <compass-settings-icon name="translate"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneOfferTranslate')}</span>
        <span class="row-detail">
          ${i18n('generalPaneOfferTranslateDetail')}
        </span>
      </span>
      <input type="checkbox" class="switch"
          data-pref="${PREF_OFFER_TRANSLATE}"
          .checked="${this.getBool_(PREF_OFFER_TRANSLATE)}"
          ?disabled="${this.isEnforced_(PREF_OFFER_TRANSLATE)}"
          @change="${this.onSwitchChange_}">
    </label>
    <label class="row control-row" data-row="spellcheck">
      <span class="tile" data-color="red">
        <compass-settings-icon name="spell"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneSpellcheck')}</span>
      </span>
      <input type="checkbox" class="switch" data-pref="${PREF_SPELLCHECK}"
          .checked="${this.getBool_(PREF_SPELLCHECK)}"
          ?disabled="${this.isEnforced_(PREF_SPELLCHECK)}"
          @change="${this.onSwitchChange_}">
    </label>
  </div>
</section>

<section class="row-group">
  <h2 class="group-title">${i18n('generalPaneSystem')}</h2>
  <div class="group">
    <div class="row control-row" data-row="hardwareAcceleration">
      <span class="tile" data-color="slate">
        <compass-settings-icon name="chip"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneHardwareAcceleration')}</span>
        <span class="row-detail" ?hidden="${!this.needsRelaunch_()}">
          ${i18n('generalPaneRelaunchNeeded')}
        </span>
      </span>
      <button class="pill accent" ?hidden="${!this.needsRelaunch_()}"
          @click="${this.onRelaunchClick_}">
        ${i18n('generalPaneRelaunch')}
      </button>
      <input type="checkbox" class="switch"
          aria-label="${i18n('generalPaneHardwareAcceleration')}"
          data-pref="${PREF_HARDWARE_ACCELERATION}"
          .checked="${this.getBool_(PREF_HARDWARE_ACCELERATION)}"
          ?disabled="${this.isEnforced_(PREF_HARDWARE_ACCELERATION)}"
          @change="${this.onSwitchChange_}">
    </div>
    <button class="row" data-row="proxy" @click="${this.onProxyClick_}">
      <span class="tile" data-color="gray">
        <compass-settings-icon name="network"></compass-settings-icon>
      </span>
      <span class="row-text">
        <span class="row-label">${i18n('generalPaneProxy')}</span>
        <span class="row-detail">${i18n('generalPaneProxyDetail')}</span>
      </span>
      <compass-settings-icon class="row-trail" name="external"
          title="${i18n('generalPaneOpensSystemSettings')}">
      </compass-settings-icon>
    </button>
  </div>
</section>
</div>
<!--_html_template_end_-->`;
  // clang-format on
}

