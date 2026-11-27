// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n} from '../i18n.js';
import type {ArchivedEntry} from '../sidebar.mojom-webui.js';

import type {CompassArchiveAppElement} from './app.js';

export function getHtml(this: CompassArchiveAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.loaded_ ? html`
<header>
  <h1>${i18n('sidebarArchived')}</h1>
  ${!this.isEmpty_() ? html`
    <button id="clear" class="pill" @click="${this.onClearClick_}">
      ${i18n('sidebarClear')}
    </button>` : nothing}
  <button id="close" title="${i18n('close')}" aria-label="${i18n('close')}"
      @click="${this.onCloseClick_}">
    <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M2 2l8 8M10 2l-8 8"></path></svg>
  </button>
</header>
<main role="dialog" aria-label="${i18n('sidebarArchivedTabs')}">
  ${this.isEmpty_() ? html`
    <div id="hint" class="card padded hint">${this.getHint_()}</div>` : html`
    <div id="list" class="card" role="list">
      ${repeat(this.entries_, (entry: ArchivedEntry) => entry.id,
               (entry: ArchivedEntry) => html`
        <div class="row" role="listitem">
          <button class="archived" data-archived-id="${entry.id}"
              title="${this.getTitle_(entry)}" @click="${this.onEntryClick_}">
            <img class="favicon" alt="" src="${this.getFaviconUrl_(entry)}">
            <span class="title">${this.getTitle_(entry)}</span>
            <span class="age">${this.getAge_(entry)}</span>
          </button>
          <button class="remove" data-archived-id="${entry.id}"
              title="${i18n('sidebarRemoveFromArchive')}"
              aria-label="${i18n('sidebarRemoveFromArchive')}"
              @click="${this.onRemoveClick_}">
            <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M3 3l6 6M9 3l-6 6"></path></svg>
          </button>
        </div>`)}
    </div>`}
</main>` : ''}
<!--_html_template_end_-->`;
  // clang-format on
}
