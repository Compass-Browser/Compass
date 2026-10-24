// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {StrataSidebarAppElement} from './app.js';

export function getHtml(this: StrataSidebarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="tabs" role="tablist">
  ${this.tabs_.map(tab => html`
    <div class="tab" role="tab" aria-selected="${tab.active}"
        ?active="${tab.active}" data-tab-id="${tab.id}"
        title="${this.getTitle_(tab)}" @click="${this.onTabClick_}">
      <img class="favicon" src="${this.getFaviconUrl_(tab)}" alt="">
      <span class="title">${this.getTitle_(tab)}</span>
      <button class="close" data-tab-id="${tab.id}" aria-label="Close tab"
          @click="${this.onCloseClick_}">&times;</button>
    </div>`)}
</div>
<button id="newTab" @click="${this.onNewTabClick_}">
  <span class="plus">+</span>New Tab
</button>
<!--_html_template_end_-->`;
  // clang-format on
}
