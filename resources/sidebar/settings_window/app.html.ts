// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF} from '../i18n.js';

import type {CompassSettingsWindowAppElement} from './app.js';

export function getHtml(this: CompassSettingsWindowAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<header id="toolbar">
  <div id="titleRow">
    <h1 id="title">${this.getTitle_()}</h1>
    <label id="search">
      <compass-settings-icon name="search"></compass-settings-icon>
      <input id="searchInput" type="search" placeholder="${i18n('settingsWindowSearch')}"
          aria-label="${i18n('settingsWindowSearchLabel')}" spellcheck="false" autocomplete="off"
          .value="${this.query_}" @input="${this.onSearchInput_}"
          @keydown="${this.onSearchKeydown_}">
    </label>
  </div>
  <nav id="tabs" role="tablist" aria-label="${i18n('settingsWindowTitle')}">
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
          ${i18nF('settingsWindowNoResults', this.getQuery_())}
        </p>
      </div>` : nothing}
    ${this.showsHeader_() ? html`
      <header class="pane-header">
        <span class="tile large" data-color="${this.pane_.color}">
          <compass-settings-icon name="${this.pane_.id}">
          </compass-settings-icon>
        </span>
        <div class="pane-header-text">
          <h2 class="pane-header-title">${this.pane_.label}</h2>
          <p class="pane-header-blurb">${this.pane_.blurb}</p>
        </div>
      </header>` : nothing}
    ${this.showsSubpageHeader_() ? html`
      <button class="back" id="subpageBack" @click="${this.onBackClick_}">
        <compass-settings-icon name="back"></compass-settings-icon>
        ${this.pane_.label}
      </button>
      <h2 class="subpage-title">${this.subpage_!.subpage!.label}</h2>` :
      nothing}
    <div id="sections" ?hidden="${this.isSearching_()}"
        ?data-single="${this.isSinglePane_()}">
    </div>
    <div id="subpageRows"
        ?hidden="${this.isSearching_() || !!this.subpage_}">
      ${this.getSubpageGroups_().map(group => html`
        <section class="row-group">
          <h2 class="group-title">${group.title}</h2>
          <div class="group">
            ${group.rows.map(row => html`
              <button class="row" data-path="${row.section.path}"
                  data-row="${row.section.path}"
                  @click="${this.onSubpageRowClick_}">
                <span class="tile" data-color="${row.subpage.color}">
                  <compass-settings-icon name="${row.subpage.icon}">
                  </compass-settings-icon>
                </span>
                <span class="row-text">
                  <span class="row-label">${row.subpage.label}</span>
                  <span class="row-detail"
                      ?hidden="${!this.getSubpageDetail_(row.section)}">
                    ${this.getSubpageDetail_(row.section)}
                  </span>
                </span>
                <compass-settings-icon class="row-trail" name="chevron">
                </compass-settings-icon>
              </button>`)}
          </div>
        </section>`)}
    </div>
    <div id="groups" ?hidden="${this.isSearching_() || !!this.subpage_}">
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
                    title="${i18n('settingsWindowOpensInTab')}">
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
