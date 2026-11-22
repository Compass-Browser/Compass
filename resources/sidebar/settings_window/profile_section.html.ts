// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsProfileSectionElement} from './profile_section.js';

export function getHtml(this: CompassSettingsProfileSectionElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<section class="row-group">
  <h2 class="group-title">This profile</h2>
  <div class="group">
    <div class="row control-row" data-row="profileName">
      <span class="row-text">
        <span class="row-label">Name</span>
      </span>
      <input class="field" type="text" aria-label="Profile name"
          maxlength="${this.maxNameLength}" spellcheck="false"
          .value="${this.getName_()}" @change="${this.onNameChange_}">
    </div>
  </div>
</section>
<!--_html_template_end_-->`;
  // clang-format on
}
