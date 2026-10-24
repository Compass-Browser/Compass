// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing, repeat} from '//resources/lit/v3_0/lit.rollup.js';

import type {StrataSidebarNodeElement} from './node.js';
import type {Node} from './sidebar.mojom-webui.js';

export function getHtml(this: StrataSidebarNodeElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.isTab_() ? html`
  <div class="tab row" role="tab" draggable="true" tabindex="-1"
      data-node-id="${this.node!.id}"
      aria-selected="${this.isActive_(this.node!)}"
      ?active="${this.isActive_(this.node!)}" ?unloaded="${!this.node!.live}"
      title="${this.getTitle_(this.node!)}" @click="${this.onTabClick_}">
    <img class="favicon" src="${this.getFaviconUrl_(this.node!)}" alt="">
    <span class="title">${this.getTitle_(this.node!)}</span>
    <button class="close" data-node-id="${this.node!.id}"
        aria-label="${this.getCloseLabel_(this.node!)}"
        @click="${this.onCloseClick_}">&times;</button>
  </div>` : nothing}
${this.isSplit_() ? html`
  <div class="split row" draggable="true" data-node-id="${this.node!.id}">
    ${repeat(this.getChildren_(), (pane: Node) => pane.id, (pane: Node) => html`
      <div class="tab pane" role="tab" tabindex="-1"
          data-node-id="${pane.id}"
          aria-selected="${this.isActive_(pane)}"
          ?active="${this.isActive_(pane)}" ?unloaded="${!pane.live}"
          title="${this.getTitle_(pane)}" @click="${this.onTabClick_}">
        <img class="favicon" src="${this.getFaviconUrl_(pane)}" alt="">
        <span class="title">${this.getTitle_(pane)}</span>
        <button class="close" data-node-id="${pane.id}"
            aria-label="${this.getCloseLabel_(pane)}"
            @click="${this.onCloseClick_}">&times;</button>
      </div>`)}
  </div>` : nothing}
${this.isFolder_() ? html`
  <div class="folder-row row" draggable="true" data-node-id="${this.node!.id}"
      role="treeitem" tabindex="-1" aria-expanded="${!this.isCollapsed_()}"
      @click="${this.onFolderClick_}">
    <span class="chevron" ?collapsed="${this.isCollapsed_()}"></span>
    ${this.editing_ ? html`
      <input class="folder-title-input" .value="${this.node!.title}"
          aria-label="Folder name" @click="${this.onTitleInputClick_}"
          @keydown="${this.onTitleKeydown_}" @blur="${this.onTitleBlur_}">` : html`
      <span class="title" @dblclick="${this.onFolderTitleDblclick_}">
        ${this.node!.title}
      </span>`}
    <button class="close" aria-label="Delete folder"
        @click="${this.onDeleteFolderClick_}">&times;</button>
  </div>
  ${!this.isCollapsed_() ? html`
    <div class="folder-children" role="group">
      ${repeat(this.getChildren_(), (child: Node) => child.id,
               (child: Node) => html`
        <strata-sidebar-node .node="${child}" .activeId="${this.activeId}"
            .collapsedIds="${this.collapsedIds}">
        </strata-sidebar-node>`)}
    </div>` : nothing}` : nothing}
<!--_html_template_end_-->`;
  // clang-format on
}
