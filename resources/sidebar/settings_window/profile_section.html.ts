// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF} from '../i18n.js';

import type {CompassSettingsProfileSectionElement} from './profile_section.js';

export function getHtml(this: CompassSettingsProfileSectionElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<section class="row-group">
  <div class="group-header">
    <h2 class="group-title">${i18n('settingsWindowPaneProfiles')}</h2>
    <button class="pill small" data-row="addProfile"
        @click="${this.onAddClick_}">
      ${i18n('profileSectionNewProfile')}
    </button>
  </div>
  <div class="group">
    ${this.profiles_.map(profile => html`
      <div class="row control-row profile-row"
          data-row="${profile.current ? 'profileName' : nothing}">
        <span class="row-text">
          <input class="field profile-name" type="text"
              aria-label="${i18nF('profileSectionNameOf', profile.name)}"
              data-profile-id="${profile.id}"
              maxlength="${this.maxNameLength}" spellcheck="false"
              .value="${profile.name}" @change="${this.onNameChange_}"
              @keydown="${this.onNameKeydown_}">
          <span class="row-detail">${this.getSpaceCountText_(profile)}</span>
        </span>
        <button class="pill danger" data-profile-id="${profile.id}"
            aria-label="${i18nF('profileSectionDeleteProfile', profile.name)}"
            ?hidden="${this.isConfirmingDelete_(profile)}"
            ?disabled="${!this.canDelete_()}"
            @click="${this.onDeleteClick_}">
          ${i18n('profileSectionDeleteEllipsis')}
        </button>
      </div>
      <div class="row control-row confirm-row"
          ?hidden="${!this.isConfirmingDelete_(profile)}">
        <span class="row-text">
          <span class="row-detail">${this.getDeleteText_(profile)}</span>
        </span>
        <button class="pill" @click="${this.onDeleteCancelClick_}">
          ${i18n('cancel')}
        </button>
        <button class="pill danger" data-profile-id="${profile.id}"
            @click="${this.onDeleteConfirmClick_}">
          ${i18n('delete')}
        </button>
      </div>`)}
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
