// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF} from '../i18n.js';

import type {CompassSettingsSearchEnginesElement} from './search_engines.js';

export function getHtml(this: CompassSettingsSearchEnginesElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  ${i18n('generalPaneTitle')}
</button>
${this.draft_ ? html`
<section class="row-group">
  <h2 class="group-title">${this.getDraftTitle_()}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text"><span class="row-label">${i18n('searchEnginesName')}</span></span>
      <input class="field" data-field="name" aria-label="${i18n('searchEnginesName')}"
          spellcheck="false" .value="${this.draft_.name}"
          @input="${this.onDraftInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">${i18n('searchEnginesShortcut')}</span>
        <span class="row-detail">${i18n('searchEnginesShortcutDetail')}</span>
      </span>
      <input class="field" data-field="keyword" aria-label="${i18n('searchEnginesShortcut')}"
          spellcheck="false" .value="${this.draft_.keyword}"
          @input="${this.onDraftInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">${i18n('searchEnginesAddress')}</span>
        <span class="row-detail">${i18n('searchEnginesAddressDetail')}</span>
      </span>
      <input class="field" data-field="url" aria-label="${i18n('searchEnginesAddress')}"
          spellcheck="false" placeholder="https://example.com/search?q=%s"
          .value="${this.draft_.url}" @input="${this.onDraftInput_}">
    </div>
  </div>
  <div class="actions">
    <span class="error" ?hidden="${!this.draft_.error}">
      ${this.draft_.error}
    </span>
    <button class="pill" @click="${this.onDraftCancelClick_}">
      ${i18n('cancel')}
    </button>
    <button class="pill accent" @click="${this.onDraftSaveClick_}">
      ${i18n('save')}
    </button>
  </div>
</section>` : nothing}

<section class="row-group">
  <h2 class="group-title">${i18n('searchEnginesTitle')}</h2>
  <div class="group">
    ${this.getEngines_(false).map(engine => html`
      <div class="row control-row" data-engine="${engine.id}">
        <span class="row-text">
          <span class="row-label">${engine.name}</span>
          <span class="row-detail">${this.getEngineDetail_(engine)}</span>
        </span>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${engine.isDefault || !engine.canMakeDefault}"
            @click="${this.onMakeEngineDefaultClick_}">
          ${i18n('generalPaneMakeDefault')}
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canEdit}" @click="${this.onEditEngineClick_}">
          ${i18n('edit')}
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canRemove}" @click="${this.onRemoveEngineClick_}">
          ${i18n('remove')}
        </button>
      </div>`)}
  </div>
</section>

<section class="row-group">
  <div class="group-header">
    <h2 class="group-title">${i18n('searchEnginesSiteSearch')}</h2>
    <button class="pill small" @click="${this.onAddEngineClick_}">
      ${i18n('searchEnginesAddEllipsis')}
    </button>
  </div>
  <div class="group">
    ${this.getEngines_(true).map(engine => html`
      <div class="row control-row" data-engine="${engine.id}">
        <input type="checkbox" class="switch" data-id="${engine.id}"
            aria-label="${i18nF('searchEnginesUse', engine.name)}" .checked="${engine.isActive}"
            ?disabled="${engine.isActive ? !engine.canDeactivate :
                                           !engine.canActivate}"
            @change="${this.onEngineActiveChange_}">
        <span class="row-text">
          <span class="row-label">${engine.name}</span>
          <span class="row-detail">${this.getEngineDetail_(engine)}</span>
        </span>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${engine.isDefault || !engine.canMakeDefault}"
            @click="${this.onMakeEngineDefaultClick_}">
          ${i18n('generalPaneMakeDefault')}
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canEdit}" @click="${this.onEditEngineClick_}">
          ${i18n('edit')}
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canRemove}" @click="${this.onRemoveEngineClick_}">
          ${i18n('remove')}
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.getEngines_(true).length > 0}">
      ${i18n('searchEnginesNoSiteSearch')}
    </p>
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
