// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {StrataSidebarNodeElement} from './node.js';

export function getHtml(this: StrataSidebarNodeElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isTab_() ? html`
  <div class="tab" role="tab" aria-selected="${this.isActive_()}"
      ?active="${this.isActive_()}" title="${this.getTitle_()}"
      @click="${this.onTabClick_}">
    <img class="favicon" src="${this.getFaviconUrl_()}" alt="">
    <span class="title">${this.getTitle_()}</span>
    <button class="close" aria-label="Close tab"
        @click="${this.onCloseClick_}">&times;</button>
  </div>` : nothing}
${this.isSplit_() ? html`
  <div class="split">
    ${this.getChildren_().map(pane => html`
      <strata-sidebar-node .node="${pane}" .activeId="${this.activeId}">
      </strata-sidebar-node>`)}
  </div>` : nothing}
${this.isFolder_() ? html`
  <div class="folder">
    <div class="folder-title">${this.node!.title}</div>
    <div class="folder-children">
      ${this.getChildren_().map(child => html`
        <strata-sidebar-node .node="${child}" .activeId="${this.activeId}">
        </strata-sidebar-node>`)}
    </div>
  </div>` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
