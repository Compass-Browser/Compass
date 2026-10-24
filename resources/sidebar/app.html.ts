// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {StrataSidebarAppElement} from './app.js';

export function getHtml(this: StrataSidebarAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="sections">
  ${this.getPinned_().length ? html`
    <div id="pinned" class="section">
      ${this.getPinned_().map(node => html`
        <strata-sidebar-node .node="${node}" .activeId="${this.activeId_}">
        </strata-sidebar-node>`)}
    </div>
    <div class="divider"></div>` : nothing}
  <div id="today" class="section" role="tablist">
    ${this.getToday_().map(node => html`
      <strata-sidebar-node .node="${node}" .activeId="${this.activeId_}">
      </strata-sidebar-node>`)}
  </div>
</div>
<button id="newTab" @click="${this.onNewTabClick_}">
  <span class="plus">+</span>New Tab
</button>
<!--_html_template_end_-->`;
  // clang-format on
}
