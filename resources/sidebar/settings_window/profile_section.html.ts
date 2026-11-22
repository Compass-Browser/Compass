// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsProfileSectionElement} from './profile_section.js';

export function getHtml(this: CompassSettingsProfileSectionElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<section class="row-group">
  <div class="group-header">
    <h2 class="group-title">Profiles</h2>
    <button class="pill small" data-row="addProfile"
        @click="${this.onAddClick_}">
      New profile
    </button>
  </div>
  <div class="group">
    ${this.profiles_.map(profile => html`
      <div class="row control-row profile-row"
          data-row="${profile.current ? 'profileName' : nothing}">
        <span class="row-text">
          <input class="field profile-name" type="text"
              aria-label="Name of ${profile.name}"
              data-profile-id="${profile.id}"
              maxlength="${this.maxNameLength}" spellcheck="false"
              .value="${profile.name}" @change="${this.onNameChange_}"
              @keydown="${this.onNameKeydown_}">
          <span class="row-detail">${this.getSpaceCountText_(profile)}</span>
        </span>
        <button class="pill danger" data-profile-id="${profile.id}"
            aria-label="Delete ${profile.name}"
            ?hidden="${this.isConfirmingDelete_(profile)}"
            ?disabled="${!this.canDelete_()}"
            @click="${this.onDeleteClick_}">
          Delete…
        </button>
      </div>
      <div class="row control-row confirm-row"
          ?hidden="${!this.isConfirmingDelete_(profile)}">
        <span class="row-text">
          <span class="row-detail">${this.getDeleteText_(profile)}</span>
        </span>
        <button class="pill" @click="${this.onDeleteCancelClick_}">
          Cancel
        </button>
        <button class="pill danger" data-profile-id="${profile.id}"
            @click="${this.onDeleteConfirmClick_}">
          Delete
        </button>
      </div>`)}
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
