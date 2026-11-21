// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsSearchEnginesElement} from './search_engines.js';

export function getHtml(this: CompassSettingsSearchEnginesElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<button class="back" @click="${this.onBackClick_}">
  <compass-settings-icon name="back"></compass-settings-icon>
  General
</button>
${this.draft_ ? html`
<section class="row-group">
  <h2 class="group-title">${this.getDraftTitle_()}</h2>
  <div class="group">
    <div class="row control-row">
      <span class="row-text"><span class="row-label">Name</span></span>
      <input class="field" data-field="name" aria-label="Name"
          spellcheck="false" .value="${this.draft_.name}"
          @input="${this.onDraftInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">Shortcut</span>
        <span class="row-detail">Type it in the command bar to search there.</span>
      </span>
      <input class="field" data-field="keyword" aria-label="Shortcut"
          spellcheck="false" .value="${this.draft_.keyword}"
          @input="${this.onDraftInput_}">
    </div>
    <div class="row control-row">
      <span class="row-text">
        <span class="row-label">Address</span>
        <span class="row-detail">With %s where the query goes.</span>
      </span>
      <input class="field" data-field="url" aria-label="Address"
          spellcheck="false" placeholder="https://example.com/search?q=%s"
          .value="${this.draft_.url}" @input="${this.onDraftInput_}">
    </div>
  </div>
  <div class="actions">
    <span class="error" ?hidden="${!this.draft_.error}">
      ${this.draft_.error}
    </span>
    <button class="pill" @click="${this.onDraftCancelClick_}">Cancel</button>
    <button class="pill accent" @click="${this.onDraftSaveClick_}">Save</button>
  </div>
</section>` : nothing}

<section class="row-group">
  <h2 class="group-title">Search engines</h2>
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
          Make Default
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canEdit}" @click="${this.onEditEngineClick_}">
          Edit
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canRemove}" @click="${this.onRemoveEngineClick_}">
          Remove
        </button>
      </div>`)}
  </div>
</section>

<section class="row-group">
  <div class="group-header">
    <h2 class="group-title">Site search</h2>
    <button class="pill small" @click="${this.onAddEngineClick_}">Add…</button>
  </div>
  <div class="group">
    ${this.getEngines_(true).map(engine => html`
      <div class="row control-row" data-engine="${engine.id}">
        <input type="checkbox" class="switch" data-id="${engine.id}"
            aria-label="Use ${engine.name}" .checked="${engine.isActive}"
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
          Make Default
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canEdit}" @click="${this.onEditEngineClick_}">
          Edit
        </button>
        <button class="pill small" data-id="${engine.id}"
            ?hidden="${!engine.canRemove}" @click="${this.onRemoveEngineClick_}">
          Remove
        </button>
      </div>`)}
    <p class="empty" ?hidden="${this.getEngines_(true).length > 0}">
      No site search yet. Sites you search on are added here.
    </p>
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
