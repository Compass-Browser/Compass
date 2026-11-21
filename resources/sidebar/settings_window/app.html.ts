// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {CompassSettingsWindowAppElement} from './app.js';

export function getHtml(this: CompassSettingsWindowAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<header id="toolbar">
  <div id="titleRow">
    <h1 id="title">${this.getTitle_()}</h1>
    <label id="search">
      <compass-settings-icon name="search"></compass-settings-icon>
      <input id="searchInput" type="search" placeholder="Search"
          aria-label="Search settings" spellcheck="false" autocomplete="off"
          .value="${this.query_}" @input="${this.onSearchInput_}"
          @keydown="${this.onSearchKeydown_}">
    </label>
  </div>
  <nav id="tabs" role="tablist" aria-label="Settings">
    ${this.panes.map(p => html`
      <button class="tab" role="tab" data-pane="${p.id}"
          aria-selected="${this.isSelected_(p)}"
          tabindex="${p === this.pane_ ? 0 : -1}"
          @click="${this.onTabClick_}" @keydown="${this.onTabKeydown_}">
        <compass-settings-icon name="${p.id}"></compass-settings-icon>
        <span>${p.label}</span>
      </button>`)}
  </nav>
</header>
<main id="scroller">
  <div id="content">
    ${this.isSearching_() ? html`
      <div id="results" class="group" role="list">
        ${this.results_.map((r, index) => html`
          <button class="row result" role="listitem" data-index="${index}"
              @click="${this.onResultClick_}">
            <compass-settings-icon class="row-icon" name="${r.pane.id}">
            </compass-settings-icon>
            <span class="row-text">
              <span class="row-label">${r.label}</span>
              <span class="row-detail">${r.detail}</span>
            </span>
            <compass-settings-icon class="row-trail" name="chevron">
            </compass-settings-icon>
          </button>`)}
        <p class="empty" ?hidden="${this.results_.length > 0}">
          No settings match “${this.getQuery_()}”.
        </p>
      </div>` : nothing}
    <div id="sections" ?hidden="${this.isSearching_()}"
        ?data-single="${this.isSinglePane_()}">
    </div>
    <div id="groups" ?hidden="${this.isSearching_()}">
      ${this.pane_.groups.map(group => html`
        <section class="row-group">
          <h2 class="group-title">${group.title}</h2>
          <div class="group">
            ${group.rows.map(row => html`
              <button class="row" data-row="${row.id}"
                  @click="${this.onRowClick_}">
                <span class="row-text">
                  <span class="row-label">${row.label}</span>
                  <span class="row-detail">${row.detail}</span>
                </span>
                <compass-settings-icon class="row-trail" name="external"
                    title="Opens in a tab">
                </compass-settings-icon>
              </button>`)}
          </div>
        </section>`)}
    </div>
  </div>
</main>
<!--_html_template_end_-->`;
  // clang-format on
}
